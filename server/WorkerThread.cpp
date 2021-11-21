/*=====================================================================
WorkerThread.cpp
------------------
Copyright Glare Technologies Limited 2018 -
=====================================================================*/
#include "WorkerThread.h"


#include "ServerWorldState.h"
#include "Server.h"
#include "Screenshot.h"
#include "SubEthTransaction.h"
#include "../shared/Protocol.h"
#include "../shared/UID.h"
#include "../shared/WorldObject.h"
#include <vec3.h>
#include <ConPrint.h>
#include <Clock.h>
#include <AESEncryption.h>
#include <SHA256.h>
#include <Base64.h>
#include <Exception.h>
#include <MySocket.h>
#include <URL.h>
#include <Lock.h>
#include <StringUtils.h>
#include <CryptoRNG.h>
#include <SocketBufferOutStream.h>
#include <PlatformUtils.h>
#include <KillThreadMessage.h>
#include <Parser.h>
#include <FileUtils.h>
#include <MemMappedFile.h>


static const bool VERBOSE = false;
static const int MAX_STRING_LEN = 10000;


WorkerThread::WorkerThread(const Reference<SocketInterface>& socket_, Server* server_)
:	socket(socket_),
	server(server_),
	scratch_packet(SocketBufferOutStream::DontUseNetworkByteOrder)
{
	//if(VERBOSE) print("event_fd.efd: " + toString(event_fd.efd));
}


WorkerThread::~WorkerThread()
{
}


static void updatePacketLengthField(SocketBufferOutStream& packet)
{
	// length field is second uint32
	assert(packet.buf.size() >= sizeof(uint32) * 2);
	if(packet.buf.size() >= sizeof(uint32) * 2)
	{
		const uint32 len = (uint32)packet.buf.size();
		std::memcpy(&packet.buf[4], &len, 4);
	}
}


static void initPacket(SocketBufferOutStream& scratch_packet, uint32 message_id)
{
	scratch_packet.buf.resize(sizeof(uint32) * 2);
	std::memcpy(&scratch_packet.buf[0], &message_id, sizeof(uint32));
	std::memset(&scratch_packet.buf[4], 0, sizeof(uint32)); // Write dummy message length, will be updated later when size of message is known.
}


void WorkerThread::sendGetFileMessageIfNeeded(const std::string& resource_URL)
{
	if(!ResourceManager::isValidURL(resource_URL))
		throw glare::Exception("Invalid URL: '" + resource_URL + "'");

	try
	{
		URL parsed_url = URL::parseURL(resource_URL);

		// If this is a web URL, then we don't need to get it from the client.
		if(parsed_url.scheme == "http" || parsed_url.scheme == "https")
			return;
	}
	catch(glare::Exception&)
	{}

	// See if we have this file on the server already
	{
		const std::string path = server->world_state->resource_manager->pathForURL(resource_URL);
		if(FileUtils::fileExists(path))
		{
			// Check hash?
			conPrint("resource file with URL '" + resource_URL + "' already present on disk.");
		}
		else
		{
			conPrint("resource file with URL '" + resource_URL + "' not present on disk, sending get file message to client.");

			// We need the file from the client.
			// Send the client a 'get file' message
			initPacket(scratch_packet, Protocol::GetFile);
			scratch_packet.writeStringLengthFirst(resource_URL);
			updatePacketLengthField(scratch_packet);

			this->enqueueDataToSend(scratch_packet);
		}
	}
}


static void writeErrorMessageToClient(SocketInterfaceRef& socket, const std::string& msg)
{
	SocketBufferOutStream packet(SocketBufferOutStream::DontUseNetworkByteOrder);
	initPacket(packet, Protocol::ErrorMessageID);
	packet.writeStringLengthFirst(msg);
	updatePacketLengthField(packet);

	socket->writeData(packet.buf.data(), packet.buf.size());
	socket->flush();
}


// Enqueues packet to WorkerThreads to send to clients connected to the server.
static void enqueuePacketToBroadcast(const SocketBufferOutStream& packet_buffer, Server* server)
{
	assert(packet_buffer.buf.size() > 0);
	if(packet_buffer.buf.size() > 0)
	{
		Lock lock(server->worker_thread_manager.getMutex());
		for(auto i = server->worker_thread_manager.getThreads().begin(); i != server->worker_thread_manager.getThreads().end(); ++i)
		{
			assert(dynamic_cast<WorkerThread*>(i->getPointer()));
			static_cast<WorkerThread*>(i->getPointer())->enqueueDataToSend(packet_buffer);
		}
	}
}


void WorkerThread::handleResourceUploadConnection()
{
	conPrint("handleResourceUploadConnection()");

	try
	{
		const std::string username = socket->readStringLengthFirst(MAX_STRING_LEN);
		const std::string password = socket->readStringLengthFirst(MAX_STRING_LEN);

		conPrint("\tusername: '" + username + "'");

		UserRef client_user;
		{
			Lock lock(server->world_state->mutex);
			auto res = server->world_state->name_to_users.find(username);
			if(res != server->world_state->name_to_users.end())
			{
				User* user = res->second.getPointer();
				if(user->isPasswordValid(password))
					client_user = user; // Password is valid, log user in.
			}
		}

		if(client_user.isNull())
		{
			conPrint("\tLogin failed.");
			socket->writeUInt32(Protocol::LogInFailure); // Note that this is not a framed message.
			socket->writeStringLengthFirst("Login failed.");

			socket->writeData(scratch_packet.buf.data(), scratch_packet.buf.size());
			return;
		}


		const std::string URL = socket->readStringLengthFirst(MAX_STRING_LEN);

		conPrint("\tURL: '" + URL + "'");

		/*if(!ResourceManager::isValidURL(URL))
		{
		conPrint("Invalid URL '" + URL + "'");
		throw glare::Exception("Invalid URL '" + URL + "'");
		}*/

		// See if we have a resource in the ResourceManager already
		ResourceRef resource = server->world_state->resource_manager->getOrCreateResourceForURL(URL); // Will create a new Resource ob if not already inserted.

		{
			Lock lock(server->world_state->mutex);
			server->world_state->addResourcesAsDBDirty(resource);
		}

		if(resource->owner_id == UserID::invalidUserID())
		{
			// No such resource existed before, client may create this resource.
		}
		else // else if resource already existed:
		{
			if(resource->owner_id != client_user->id) // If this resource already exists and was created by someone else:
			{
				socket->writeUInt32(Protocol::NoWritePermissions); // Note that this is not a framed message.
				socket->writeStringLengthFirst("Not allowed to upload resource to URL '" + URL + ", someone else created a resource at this URL already.");
				return;
			}
		}
		
		// resource->setState(Resource::State_Transferring); // Don't set this (for now) or we will have to handle changing it on exceptions below.


		const uint64 file_len = socket->readUInt64();
		conPrint("\tfile_len: " + toString(file_len) + " B");
		if(file_len == 0)
		{
			socket->writeUInt32(Protocol::InvalidFileSize); // Note that this is not a framed message.
			socket->writeStringLengthFirst("Invalid file len of zero.");
			return;
		}

		// TODO: cap length in a better way
		if(file_len > 1000000000)
		{
			socket->writeUInt32(Protocol::InvalidFileSize); // Note that this is not a framed message.
			socket->writeStringLengthFirst("uploaded file too large.");
			return;
		}

		// Otherwise upload is allowed:
		socket->writeUInt32(Protocol::UploadAllowed);

		std::vector<uint8> buf(file_len);
		socket->readData(buf.data(), file_len);

		conPrint("\tReceived file with URL '" + URL + "' from client. (" + toString(file_len) + " B)");

		// Save to disk
		const std::string local_path = server->world_state->resource_manager->pathForURL(URL);

		conPrint("\tWriting to disk at '" + local_path + "'...");

		FileUtils::writeEntireFile(local_path, (const char*)buf.data(), buf.size());

		conPrint("\tWritten to disk.");

		resource->owner_id = client_user->id;
		resource->setState(Resource::State_Present);

		{
			Lock lock(server->world_state->mutex);
			server->world_state->addResourcesAsDBDirty(resource);
		}

		// Send NewResourceOnServer message to connected clients
		{
			initPacket(scratch_packet, Protocol::NewResourceOnServer);
			scratch_packet.writeStringLengthFirst(URL);
			updatePacketLengthField(scratch_packet);

			enqueuePacketToBroadcast(scratch_packet, server);
		}

		// Connection will be closed by the client after the client has uploaded the file.  Wait for the connection to close.
		//socket->waitForGracefulDisconnect();
	}
	catch(MySocketExcep& e)
	{
		if(e.excepType() == MySocketExcep::ExcepType_ConnectionClosedGracefully)
			conPrint("Resource upload client from " + IPAddress::formatIPAddressAndPort(socket->getOtherEndIPAddress(), socket->getOtherEndPort()) + " closed connection gracefully.");
		else
			conPrint("Socket error: " + e.what());
	}
	catch(glare::Exception& e)
	{
		conPrint("glare::Exception: " + e.what());
	}
	catch(std::bad_alloc&)
	{
		conPrint("WorkerThread: Caught std::bad_alloc.");
	}
}


void WorkerThread::handleResourceDownloadConnection()
{
	conPrint("handleResourceDownloadConnection()");

	try
	{

		while(1)
		{
			const uint32 msg_type = socket->readUInt32();
			if(msg_type == Protocol::GetFiles)
			{
				conPrint("------GetFiles-----");

				const uint64 num_resources = socket->readUInt64();
				conPrint("\tnum_resources requested: " + toString(num_resources));

				for(size_t i=0; i<num_resources; ++i)
				{
					const std::string URL = socket->readStringLengthFirst(MAX_STRING_LEN);

					conPrint("\tRequested URL: '" + URL + "'");

					if(!ResourceManager::isValidURL(URL))
					{
						conPrint("\tRequested URL was invalid.");
						socket->writeUInt32(1); // write error msg to client
					}
					else
					{
						conPrint("\tRequested URL was valid.");

						const ResourceRef resource = server->world_state->resource_manager->getExistingResourceForURL(URL);
						if(resource.isNull() || (resource->getState() != Resource::State_Present))
						{
							conPrint("\tRequested URL was not present on disk.");
							socket->writeUInt32(1); // write error msg to client
						}
						else
						{
							const std::string local_path = resource->getLocalPath();

							conPrint("\tlocal path: '" + local_path + "'");

							try
							{
								// Load resource off disk
								MemMappedFile file(local_path);
								conPrint("\tSending file to client.");
								socket->writeUInt32(0); // write OK msg to client
								socket->writeUInt64(file.fileSize()); // Write file size
								socket->writeData(file.fileData(), file.fileSize()); // Write file data

								conPrint("\tSent file '" + local_path + "' to client. (" + toString(file.fileSize()) + " B)");
							}
							catch(glare::Exception& e)
							{
								conPrint("\tException while trying to load file for URL: " + e.what());

								socket->writeUInt32(1); // write error msg to client
							}
						}
					}
				}
			}
			else
			{
				conPrint("handleResourceDownloadConnection(): Unhandled msg type: " + toString(msg_type));
				return;
			}
		}
	}
	catch(MySocketExcep& e)
	{
		if(e.excepType() == MySocketExcep::ExcepType_ConnectionClosedGracefully)
			conPrint("Resource download client from " + IPAddress::formatIPAddressAndPort(socket->getOtherEndIPAddress(), socket->getOtherEndPort()) + " closed connection gracefully.");
		else
			conPrint("Socket error: " + e.what());
	}
	catch(glare::Exception& e)
	{
		conPrint("glare::Exception: " + e.what());
	}
	catch(std::bad_alloc&)
	{
		conPrint("WorkerThread: Caught std::bad_alloc.");
	}
}


void WorkerThread::handleScreenshotBotConnection()
{
	conPrint("handleScreenshotBotConnection()");

	// TODO: authentication

	try
	{
		while(1)
		{
			// Poll server state for a screenshot request
			ScreenshotRef screenshot;

			{ // lock scope
				Lock lock(server->world_state->mutex);

				server->world_state->last_screenshot_bot_contact_time = TimeStamp::currentTime();

				// Find first screenshot in screenshots map in ScreenshotState_notdone state.  NOTE: slow linear scan.
				for(auto it = server->world_state->screenshots.begin(); it != server->world_state->screenshots.end(); ++it)
				{
					if(it->second->state == Screenshot::ScreenshotState_notdone)
					{
						screenshot = it->second;
						break;
					}
				}

				if(screenshot.isNull())
				{
					// Find first screenshot in map_tile_info map in ScreenshotState_notdone state.  NOTE: slow linear scan.
					for(auto it = server->world_state->map_tile_info.info.begin(); it != server->world_state->map_tile_info.info.end(); ++it)
					{
						TileInfo& tile_info = it->second;
						if(tile_info.cur_tile_screenshot.nonNull() && tile_info.cur_tile_screenshot->state == Screenshot::ScreenshotState_notdone)
						{
							screenshot = tile_info.cur_tile_screenshot;
							break;
						}
					}
				}
			} // End lock scope

			if(screenshot.nonNull()) // If there is a screenshot to take:
			{
				if(!screenshot->is_map_tile)
				{
					socket->writeUInt32(Protocol::ScreenShotRequest);

					socket->writeDouble(screenshot->cam_pos.x);
					socket->writeDouble(screenshot->cam_pos.y);
					socket->writeDouble(screenshot->cam_pos.z);
					socket->writeDouble(screenshot->cam_angles.x);
					socket->writeDouble(screenshot->cam_angles.y);
					socket->writeDouble(screenshot->cam_angles.z);
					socket->writeInt32(screenshot->width_px);
					socket->writeInt32(screenshot->highlight_parcel_id);
				}
				else
				{
					socket->writeUInt32(Protocol::TileScreenShotRequest);

					socket->writeInt32(screenshot->tile_x);
					socket->writeInt32(screenshot->tile_y);
					socket->writeInt32(screenshot->tile_z);
				}

				// Read response
				const uint32 result = socket->readUInt32();
				if(result == Protocol::ScreenShotSucceeded)
				{
					// Read screenshot data
					const uint64 data_len = socket->readUInt64();
					if(data_len > 100000000) // ~100MB
						throw glare::Exception("data_len was too large");

					conPrint("Receiving screenshot of " + toString(data_len) + " B");
					std::vector<uint8> data(data_len);
					socket->readData(data.data(), data_len);

					conPrint("Received screenshot of " + toString(data_len) + " B");

					// Generate random path
					const int NUM_BYTES = 16;
					uint8 pathdata[NUM_BYTES];
					CryptoRNG::getRandomBytes(pathdata, NUM_BYTES);
					const std::string screenshot_filename = "screenshot_" + StringUtils::convertByteArrayToHexString(pathdata, NUM_BYTES) + ".jpg";
					const std::string screenshot_path = server->screenshot_dir + "/" + screenshot_filename;

					// Save screenshot to path
					FileUtils::writeEntireFile(screenshot_path, data);

					conPrint("Saved to disk at " + screenshot_path);

					screenshot->state = Screenshot::ScreenshotState_done;
					screenshot->local_path = screenshot_path;
					server->world_state->addScreenshotAsDBDirty(screenshot);
				}
				else
					throw glare::Exception("Client reported screenshot taking failed.");
			}
			else
			{
				socket->writeUInt32(Protocol::KeepAlive); // Send a keepalive message just to check the socket is still connected.

				// There is no current screenshot request, sleep for a while
				PlatformUtils::Sleep(10000);
			}
		}
	}
	catch(glare::Exception& e)
	{
		conPrint("handleScreenshotBotConnection: glare::Exception: " + e.what());
	}
	catch(std::exception& e)
	{
		conPrint(std::string("handleScreenshotBotConnection: Caught std::exception: ") + e.what());
	}
}


void WorkerThread::handleEthBotConnection()
{
	conPrint("handleEthBotConnection()");

	try
	{
		// Do authentication
		const std::string password = socket->readStringLengthFirst(10000);
		if(SHA256::hash(password) != StringUtils::convertHexToBinary("9bd7674cb1e7ec496f88b31264aaa3ff75ce9d60aabc5e6fd0f8e7ba8a27f829")) // See ethBotTests().
			throw glare::Exception("Invalid password");
			
		while(1)
		{
			// Poll server state for a request
			SubEthTransactionRef trans;
			uint64 largest_nonce_used = 0; 
			{ // lock scope
				Lock lock(server->world_state->mutex);

				server->world_state->last_eth_bot_contact_time = TimeStamp::currentTime();

				// Find first transction in New state.  NOTE: slow linear scan.
				for(auto it = server->world_state->sub_eth_transactions.begin(); it != server->world_state->sub_eth_transactions.end(); ++it)
				{
					if(it->second->state == SubEthTransaction::State_New)
					{
						trans = it->second;
						break;
					}
				}

				// Work out nonce to use for this transaction.  First, work out largest nonce used for succesfully submitted transactions
				for(auto it = server->world_state->sub_eth_transactions.begin(); it != server->world_state->sub_eth_transactions.end(); ++it)
				{
					if(it->second->state == SubEthTransaction::State_Completed)
						largest_nonce_used = myMax(largest_nonce_used, it->second->nonce);
				}
			} // End lock scope

			const uint64 next_nonce = myMax((uint64)server->world_state->eth_info.min_next_nonce, largest_nonce_used + 1); // min_next_nonce is to reflect any existing transactions on account

			if(trans.nonNull()) // If there is a transaction to submit:
			{
				socket->writeUInt32(Protocol::SubmitEthTransactionRequest);

				// Update transaction nonce and submitted_time
				{ // lock scope
					Lock lock(server->world_state->mutex);

					trans->nonce = next_nonce; 
					trans->submitted_time = TimeStamp::currentTime();

					server->world_state->addSubEthTransactionAsDBDirty(trans);
				}
				
				writeToStream(*trans, *socket);

				// Read response
				const uint32 result = socket->readUInt32();
				if(result == Protocol::EthTransactionSubmitted)
				{
					const UInt256 transaction_hash = readUInt256FromStream(*socket);

					conPrint("Transaction was submitted.");

					// Mark parcel as minted as an NFT
					{ // lock scope
						Lock lock(server->world_state->mutex);

						trans->state = SubEthTransaction::State_Completed; // State_Submitted;
						trans->transaction_hash = transaction_hash;

						server->world_state->addSubEthTransactionAsDBDirty(trans);

						auto parcel_res = server->world_state->getRootWorldState()->parcels.find(trans->parcel_id);
						if(parcel_res != server->world_state->getRootWorldState()->parcels.end())
						{
							Parcel* parcel = parcel_res->second.ptr();
							parcel->nft_status = Parcel::NFTStatus_MintedNFT;
							server->world_state->getRootWorldState()->addParcelAsDBDirty(parcel);
							server->world_state->markAsChanged();
						}
					} // End lock scope
				}
				else if(result == Protocol::EthTransactionSubmissionFailed)
				{
					conPrint("Transaction submission failed.");

					const std::string submission_error_message = socket->readStringLengthFirst(10000);

					{ // lock scope
						Lock lock(server->world_state->mutex);

						trans->state = SubEthTransaction::State_Submitted;
						trans->transaction_hash = UInt256(0);
						trans->submission_error_message = submission_error_message;

						server->world_state->addSubEthTransactionAsDBDirty(trans);
					}
				}
				else
					throw glare::Exception("Client reported transaction submission failed.");
			}
			else
			{
				socket->writeUInt32(Protocol::KeepAlive); // Send a keepalive message just to check the socket is still connected.

				// There is no current transaction to process, sleep for a while
				PlatformUtils::Sleep(10000);
			}
		}
	}
	catch(glare::Exception& e)
	{
		conPrint("handleEthBotConnection: glare::Exception: " + e.what());
	}
	catch(std::exception& e)
	{
		conPrint(std::string("handleEthBotConnection: Caught std::exception: ") + e.what());
	}
}


static bool objectIsInParcelOwnedByLoggedInUser(const WorldObject& ob, const User& user, ServerWorldState& world_state)
{
	assert(user.id.valid());

	for(auto& it : world_state.parcels)
	{
		const Parcel* parcel = it.second.ptr();
		if((parcel->owner_id == user.id) && parcel->pointInParcel(ob.pos))
			return true;
	}

	return false;
}


// NOTE: world state mutex should be locked before calling this method.
static bool userHasObjectWritePermissions(const WorldObject& ob, const User& user, const std::string& connected_world_name, ServerWorldState& world_state)
{
	if(user.id.valid())
	{
		return (user.id == ob.creator_id) || // If the user created/owns the object
			isGodUser(user.id) || // or if the user is the god user (id 0)
			user.name == "lightmapperbot" || // lightmapper bot has full write permissions for now.
			((connected_world_name != "") && (user.name == connected_world_name)) || // or if this is the user's personal world
			objectIsInParcelOwnedByLoggedInUser(ob, user, world_state); // Can modify objects owned by other people if they are in parcels you own.
	}
	else
		return false;
}


void WorkerThread::doRun()
{
	PlatformUtils::setCurrentThreadNameIfTestsEnabled("WorkerThread");

	ServerAllWorldsState* world_state = server->world_state.getPointer();

	UID client_avatar_uid(0);
	Reference<User> client_user; // Will be a null reference if client is not logged in, otherwise will refer to the user account the client is logged in to.
	Reference<ServerWorldState> cur_world_state; // World the client is connected to.
	bool logged_in_user_is_lightmapper_bot = false; // Just for updating the last_lightmapper_bot_contact_time.

	try
	{
		// Read hello bytes
		const uint32 hello = socket->readUInt32();
		printVar(hello);
		if(hello != Protocol::CyberspaceHello)
			throw glare::Exception("Received invalid hello message (" + toString(hello) + ") from client.");
		
		// Write hello response
		socket->writeUInt32(Protocol::CyberspaceHello);

		// Read protocol version
		const uint32 client_version = socket->readUInt32();
		printVar(client_version);
		if(client_version < Protocol::CyberspaceProtocolVersion)
		{
			socket->writeUInt32(Protocol::ClientProtocolTooOld);
			socket->writeStringLengthFirst("Sorry, your client protocol version (" + toString(client_version) + ") is too old, require version " + 
				toString(Protocol::CyberspaceProtocolVersion) + ".  Please update your client at substrata.info.");
		}
		else if(client_version > Protocol::CyberspaceProtocolVersion)
		{
			socket->writeUInt32(Protocol::ClientProtocolTooNew);
			socket->writeStringLengthFirst("Sorry, your client protocol version (" + toString(client_version) + ") is too new, require version " + 
				toString(Protocol::CyberspaceProtocolVersion) + ".  Please use an older client.");
		}
		else
		{
			socket->writeUInt32(Protocol::ClientProtocolOK);
		}

		const uint32 connection_type = socket->readUInt32();
	
		if(connection_type == Protocol::ConnectionTypeUploadResource)
		{
			handleResourceUploadConnection();
			return;
		}
		else if(connection_type == Protocol::ConnectionTypeDownloadResources)
		{
			handleResourceDownloadConnection();
			return;
		}
		else if(connection_type == Protocol::ConnectionTypeScreenshotBot)
		{
			handleScreenshotBotConnection();
			return;
		}
		else if(connection_type == Protocol::ConnectionTypeEthBot)
		{
			handleEthBotConnection();
			return;
		}
		else if(connection_type == Protocol::ConnectionTypeUpdates)
		{
			// Read name of world to connect to
			const std::string world_name = socket->readStringLengthFirst(1000);
			this->connected_world_name = world_name;

			{
				Lock lock(world_state->mutex);
				// Create world if didn't exist before.
				// TODO: do this here? or restrict possible world names to those of users etc..?
				if(world_state->world_states[world_name].isNull())
					world_state->world_states[world_name] = new ServerWorldState();
				cur_world_state = world_state->world_states[world_name];
			}

			// Write avatar UID assigned to the connected client.
			client_avatar_uid = world_state->getNextAvatarUID();
			writeToStream(client_avatar_uid, *socket);

			// Send TimeSyncMessage packet to client
			{
				initPacket(scratch_packet, Protocol::TimeSyncMessage);
				scratch_packet.writeDouble(server->getCurrentGlobalTime());
				updatePacketLengthField(scratch_packet);
				socket->writeData(scratch_packet.buf.data(), scratch_packet.buf.size());
			}

			// Send all current avatar state data to client
			{
				SocketBufferOutStream packet(SocketBufferOutStream::DontUseNetworkByteOrder);

				{ // Lock scope
					Lock lock(world_state->mutex);
					for(auto it = cur_world_state->avatars.begin(); it != cur_world_state->avatars.end(); ++it)
					{
						const Avatar* avatar = it->second.getPointer();

						// Write AvatarIsHere message
						initPacket(scratch_packet, Protocol::AvatarIsHere);
						writeToNetworkStream(*avatar, scratch_packet);
						updatePacketLengthField(scratch_packet);

						packet.writeData(scratch_packet.buf.data(), scratch_packet.buf.size());
					}
				} // End lock scope

				socket->writeData(packet.buf.data(), packet.buf.size());
			}

			// Send all current object data to client
			/*{
				Lock lock(world_state->mutex);
				for(auto it = cur_world_state->objects.begin(); it != cur_world_state->objects.end(); ++it)
				{
					const WorldObject* ob = it->second.getPointer();

					// Send ObjectCreated packet
					SocketBufferOutStream packet(SocketBufferOutStream::DontUseNetworkByteOrder);
					packet.writeUInt32(Protocol::ObjectCreated);
					ob->writeToNetworkStream(packet);
					socket->writeData(packet.buf.data(), packet.buf.size());
				}
			}*/

			// Send all current parcel data to client
			{
				SocketBufferOutStream packet(SocketBufferOutStream::DontUseNetworkByteOrder);

				{ // Lock scope
					Lock lock(world_state->mutex);
					for(auto it = cur_world_state->parcels.begin(); it != cur_world_state->parcels.end(); ++it)
					{
						const Parcel* parcel = it->second.getPointer();

						// Send ParcelCreated message
						initPacket(scratch_packet, Protocol::ParcelCreated);
						writeToNetworkStream(*parcel, scratch_packet);
						updatePacketLengthField(scratch_packet);

						packet.writeData(scratch_packet.buf.data(), scratch_packet.buf.size());
					}
				} // End lock scope

				socket->writeData(packet.buf.data(), packet.buf.size());
				socket->flush();
			}

			// Send a message saying we have sent all initial state
			/*{
				SocketBufferOutStream packet(SocketBufferOutStream::DontUseNetworkByteOrder);
				packet.writeUInt32(Protocol::InitialStateSent);
				socket->writeData(packet.buf.data(), packet.buf.size());
			}*/
		}

		assert(cur_world_state.nonNull());


		socket->setNoDelayEnabled(true); // We want to send out lots of little packets with low latency.  So disable Nagle's algorithm, e.g. send coalescing.

		
		while(1) // write to / read from socket loop
		{
			// See if we have any pending data to send in the data_to_send queue, and if so, send all pending data.
			if(VERBOSE) conPrint("WorkerThread: checking for pending data to send...");

			// We don't want to do network writes while holding the data_to_send_mutex.  So copy to temp_data_to_send.
			{
				Lock lock(data_to_send_mutex);
				temp_data_to_send = data_to_send;
				data_to_send.clear();
			}

			if(temp_data_to_send.nonEmpty() && (connection_type == Protocol::ConnectionTypeUpdates))
			{
				socket->writeData(temp_data_to_send.data(), temp_data_to_send.size());
				socket->flush();
				temp_data_to_send.clear();
			}


			if(logged_in_user_is_lightmapper_bot)
				server->world_state->last_lightmapper_bot_contact_time = TimeStamp::currentTime(); // bit of a hack


#if defined(_WIN32) || defined(OSX)
			if(socket->readable(0.05)) // If socket has some data to read from it:
#else
			if(socket->readable(event_fd)) // Block until either the socket is readable or the event fd is signalled, which means we have data to write.
#endif
			{
				// Read msg type and length
				uint32 msg_type_and_len[2];
				socket->readData(msg_type_and_len, sizeof(uint32) * 2);
				const uint32 msg_type = msg_type_and_len[0];
				const uint32 msg_len = msg_type_and_len[1];

				if((msg_len < sizeof(uint32) * 2) || (msg_len > 1000000))
					throw glare::Exception("Invalid message size: " + toString(msg_len));

				// conPrint("WorkerThread: Read message header: id: " + toString(msg_type) + ", len: " + toString(msg_len));

				// Read entire message
				msg_buffer.buf.resizeNoCopy(msg_len);
				msg_buffer.read_index = sizeof(uint32) * 2;

				socket->readData(msg_buffer.buf.data() + sizeof(uint32) * 2, msg_len - sizeof(uint32) * 2); // Read rest of message, store in msg_buffer.

				switch(msg_type)
				{
				case Protocol::AvatarTransformUpdate:
					{
						//conPrint("AvatarTransformUpdate");
						const UID avatar_uid = readUIDFromStream(msg_buffer);
						const Vec3d pos = readVec3FromStream<double>(msg_buffer);
						const Vec3f rotation = readVec3FromStream<float>(msg_buffer);
						const uint32 anim_state = msg_buffer.readUInt32();

						// Look up existing avatar in world state
						{
							Lock lock(world_state->mutex);
							auto res = cur_world_state->avatars.find(avatar_uid);
							if(res != cur_world_state->avatars.end())
							{
								Avatar* avatar = res->second.getPointer();
								avatar->pos = pos;
								avatar->rotation = rotation;
								avatar->anim_state = anim_state;
								avatar->transform_dirty = true;

								//conPrint("updated avatar transform");
							}
						}
						break;
					}
				case Protocol::AvatarPerformGesture:
					{
						//conPrint("AvatarPerformGesture");
						const UID avatar_uid = readUIDFromStream(msg_buffer);
						const std::string gesture_name = msg_buffer.readStringLengthFirst(10000);

						//conPrint("Received AvatarPerformGesture: '" + gesture_name + "'");

						if(client_user.isNull())
						{
							writeErrorMessageToClient(socket, "You must be logged in to perform a gesture.");
						}
						else
						{
							// Enqueue AvatarPerformGesture messages to worker threads to send
							initPacket(scratch_packet, Protocol::AvatarPerformGesture);
							writeToStream(avatar_uid, scratch_packet);
							scratch_packet.writeStringLengthFirst(gesture_name);
							updatePacketLengthField(scratch_packet);

							enqueuePacketToBroadcast(scratch_packet, server);
						}
						break;
					}
				case Protocol::AvatarStopGesture:
					{
						//conPrint("AvatarStopGesture");
						const UID avatar_uid = readUIDFromStream(msg_buffer);

						if(client_user.isNull())
						{
							writeErrorMessageToClient(socket, "You must be logged in to stop a gesture.");
						}
						else
						{
							// Enqueue AvatarStopGesture messages to worker threads to send
							initPacket(scratch_packet, Protocol::AvatarStopGesture);
							writeToStream(avatar_uid, scratch_packet);
							updatePacketLengthField(scratch_packet);

							enqueuePacketToBroadcast(scratch_packet, server);
						}
						break;
				}
				case Protocol::AvatarFullUpdate:
					{
						conPrint("Protocol::AvatarFullUpdate");
						const UID avatar_uid = readUIDFromStream(msg_buffer);

						Avatar temp_avatar;
						readFromNetworkStreamGivenUID(msg_buffer, temp_avatar); // Read message data before grabbing lock

						// Look up existing avatar in world state
						{
							Lock lock(world_state->mutex);
							auto res = cur_world_state->avatars.find(avatar_uid);
							if(res != cur_world_state->avatars.end())
							{
								Avatar* avatar = res->second.getPointer();
								avatar->copyNetworkStateFrom(temp_avatar);
								avatar->other_dirty = true;


								// Store avatar settings in the user data
								if(client_user.nonNull())
								{
									client_user->avatar_settings = avatar->avatar_settings;

									world_state->addUserAsDBDirty(client_user); // TODO: only do this if avatar settings actually changed.

									conPrint("Updated user avatar settings.  model_url: " + client_user->avatar_settings.model_url);
								}

								//conPrint("updated avatar transform");
							}
						}

						if(!temp_avatar.avatar_settings.model_url.empty())
							sendGetFileMessageIfNeeded(temp_avatar.avatar_settings.model_url);

						// Process resources
						std::set<std::string> URLs;
						temp_avatar.getDependencyURLSetForAllLODLevels(URLs);
						for(auto it = URLs.begin(); it != URLs.end(); ++it)
							sendGetFileMessageIfNeeded(*it);

						break;
					}
				case Protocol::CreateAvatar:
					{
						conPrint("received Protocol::CreateAvatar");
						// Note: name will come from user account
						// will use the client_avatar_uid that we assigned to the client
						
						Avatar temp_avatar;
						temp_avatar.uid = readUIDFromStream(msg_buffer); // Will be replaced.
						readFromNetworkStreamGivenUID(msg_buffer, temp_avatar); // Read message data before grabbing lock

						temp_avatar.name = client_user.isNull() ? "Anonymous" : client_user->name;

						const UID use_avatar_uid = client_avatar_uid;
						temp_avatar.uid = use_avatar_uid;

						// Look up existing avatar in world state
						{
							Lock lock(world_state->mutex);
							auto res = cur_world_state->avatars.find(use_avatar_uid);
							if(res == cur_world_state->avatars.end())
							{
								// Avatar for UID not already created, create it now.
								AvatarRef avatar = new Avatar();
								avatar->uid = use_avatar_uid;
								avatar->copyNetworkStateFrom(temp_avatar);
								avatar->state = Avatar::State_JustCreated;
								avatar->other_dirty = true;
								cur_world_state->avatars.insert(std::make_pair(use_avatar_uid, avatar));

								conPrint("created new avatar");
							}
						}

						if(!temp_avatar.avatar_settings.model_url.empty())
							sendGetFileMessageIfNeeded(temp_avatar.avatar_settings.model_url);

						// Process resources
						std::set<std::string> URLs;
						temp_avatar.getDependencyURLSetForAllLODLevels(URLs);
						for(auto it = URLs.begin(); it != URLs.end(); ++it)
							sendGetFileMessageIfNeeded(*it);

						conPrint("New Avatar creation: username: '" + temp_avatar.name + "', model_url: '" + temp_avatar.avatar_settings.model_url + "'");

						break;
					}
				case Protocol::AvatarDestroyed:
					{
						conPrint("AvatarDestroyed");
						const UID avatar_uid = readUIDFromStream(msg_buffer);

						// Mark avatar as dead
						{
							Lock lock(world_state->mutex);
							auto res = cur_world_state->avatars.find(avatar_uid);
							if(res != cur_world_state->avatars.end())
							{
								Avatar* avatar = res->second.getPointer();
								avatar->state = Avatar::State_Dead;
								avatar->other_dirty = true;
							}
						}
						break;
					}
				case Protocol::ObjectTransformUpdate:
					{
						//conPrint("received ObjectTransformUpdate");
						const UID object_uid = readUIDFromStream(msg_buffer);
						const Vec3d pos = readVec3FromStream<double>(msg_buffer);
						const Vec3f axis = readVec3FromStream<float>(msg_buffer);
						const float angle = msg_buffer.readFloat();

						// If client is not logged in, refuse object modification.
						if(client_user.isNull())
						{
							writeErrorMessageToClient(socket, "You must be logged in to modify an object.");
						}
						else
						{
							std::string err_msg_to_client;
							// Look up existing object in world state
							{
								Lock lock(world_state->mutex);
								auto res = cur_world_state->objects.find(object_uid);
								if(res != cur_world_state->objects.end())
								{
									WorldObject* ob = res->second.getPointer();

									// See if the user has permissions to alter this object:
									if(!userHasObjectWritePermissions(*ob, *client_user, this->connected_world_name, *cur_world_state))
										err_msg_to_client = "You must be the owner of this object to change it.";
									else
									{
										ob->pos = pos;
										ob->axis = axis;
										ob->angle = angle;
										ob->from_remote_transform_dirty = true;
										cur_world_state->addWorldObjectAsDBDirty(ob);
										cur_world_state->dirty_from_remote_objects.insert(ob);

										world_state->markAsChanged();
									}

									//conPrint("updated object transform");
								}
							} // End lock scope

							if(!err_msg_to_client.empty())
								writeErrorMessageToClient(socket, err_msg_to_client);
						}

						break;
					}
				case Protocol::ObjectFullUpdate:
					{
						//conPrint("received ObjectFullUpdate");
						const UID object_uid = readUIDFromStream(msg_buffer);

						WorldObject temp_ob;
						readFromNetworkStreamGivenUID(msg_buffer, temp_ob); // Read rest of ObjectFullUpdate message.

						// If client is not logged in, refuse object modification.
						if(client_user.isNull())
						{
							writeErrorMessageToClient(socket, "You must be logged in to modify an object.");
						}
						else
						{
							// Look up existing object in world state
							bool send_must_be_owner_msg = false;
							{
								Lock lock(world_state->mutex);
								auto res = cur_world_state->objects.find(object_uid);
								if(res != cur_world_state->objects.end())
								{
									WorldObject* ob = res->second.getPointer();

									// See if the user has permissions to alter this object:
									if(!userHasObjectWritePermissions(*ob, *client_user, this->connected_world_name, *cur_world_state))
									{
										send_must_be_owner_msg = true;
									}
									else
									{
										ob->copyNetworkStateFrom(temp_ob);

										ob->from_remote_other_dirty = true;
										cur_world_state->addWorldObjectAsDBDirty(ob);
										cur_world_state->dirty_from_remote_objects.insert(ob);

										world_state->markAsChanged();

										// Process resources
										std::set<std::string> URLs;
										ob->getDependencyURLSetForAllLODLevels(URLs);
										for(auto it = URLs.begin(); it != URLs.end(); ++it)
											sendGetFileMessageIfNeeded(*it);
									}
								}
							} // End lock scope

							if(send_must_be_owner_msg)
								writeErrorMessageToClient(socket, "You must be the owner of this object to change it.");
						}
						break;
					}
				case Protocol::ObjectLightmapURLChanged:
					{
						//conPrint("ObjectLightmapURLChanged");
						const UID object_uid = readUIDFromStream(msg_buffer);
						const std::string new_lightmap_url = msg_buffer.readStringLengthFirst(10000);

						// Look up existing object in world state
						{
							Lock lock(world_state->mutex);
							auto res = cur_world_state->objects.find(object_uid);
							if(res != cur_world_state->objects.end())
							{
								WorldObject* ob = res->second.getPointer();
					
								ob->lightmap_url = new_lightmap_url;

								ob->from_remote_lightmap_url_dirty = true;
								cur_world_state->addWorldObjectAsDBDirty(ob);
								cur_world_state->dirty_from_remote_objects.insert(ob);

								world_state->markAsChanged();
							}
						}
						break;
					}
				case Protocol::ObjectModelURLChanged:
					{
						//conPrint("ObjectModelURLChanged");
						const UID object_uid = readUIDFromStream(msg_buffer);
						const std::string new_model_url = msg_buffer.readStringLengthFirst(10000);

						// Look up existing object in world state
						{
							Lock lock(world_state->mutex);
							auto res = cur_world_state->objects.find(object_uid);
							if(res != cur_world_state->objects.end())
							{
								WorldObject* ob = res->second.getPointer();
					
								ob->model_url = new_model_url;

								ob->from_remote_model_url_dirty = true;
								cur_world_state->addWorldObjectAsDBDirty(ob);
								cur_world_state->dirty_from_remote_objects.insert(ob);

								world_state->markAsChanged();
							}
						}
						break;
					}
				case Protocol::ObjectFlagsChanged:
					{
						//conPrint("ObjectFlagsChanged");
						const UID object_uid = readUIDFromStream(msg_buffer);
						const uint32 flags = msg_buffer.readUInt32();

						// Look up existing object in world state
						{
							Lock lock(world_state->mutex);
							auto res = cur_world_state->objects.find(object_uid);
							if(res != cur_world_state->objects.end())
							{
								WorldObject* ob = res->second.getPointer();

								ob->flags = flags; // Copy flags

								ob->from_remote_flags_dirty = true;
								cur_world_state->addWorldObjectAsDBDirty(ob);
								cur_world_state->dirty_from_remote_objects.insert(ob);

								world_state->markAsChanged();
							}
						}
						break;
					}
				case Protocol::CreateObject: // Client wants to create an object
					{
						conPrint("CreateObject");

						WorldObjectRef new_ob = new WorldObject();
						new_ob->uid = readUIDFromStream(msg_buffer); // Read dummy UID
						readFromNetworkStreamGivenUID(msg_buffer, *new_ob);

						conPrint("model_url: '" + new_ob->model_url + "', pos: " + new_ob->pos.toString());

						// If client is not logged in, refuse object creation.
						if(client_user.isNull())
						{
							conPrint("Creation denied, user was not logged in.");
							initPacket(scratch_packet, Protocol::ErrorMessageID);
							scratch_packet.writeStringLengthFirst("You must be logged in to create an object.");
							updatePacketLengthField(scratch_packet);
							socket->writeData(scratch_packet.buf.data(), scratch_packet.buf.size());
							socket->flush();
						}
						else
						{
							new_ob->creator_id = client_user->id;
							new_ob->created_time = TimeStamp::currentTime();
							new_ob->creator_name = client_user->name;

							std::set<std::string> URLs;
							new_ob->getDependencyURLSetForAllLODLevels(URLs);
							for(auto it = URLs.begin(); it != URLs.end(); ++it)
								sendGetFileMessageIfNeeded(*it);

							// Look up existing object in world state
							{
								::Lock lock(world_state->mutex);

								new_ob->uid = world_state->getNextObjectUID();
								new_ob->state = WorldObject::State_JustCreated;
								new_ob->from_remote_other_dirty = true;
								cur_world_state->addWorldObjectAsDBDirty(new_ob);
								cur_world_state->dirty_from_remote_objects.insert(new_ob);
								cur_world_state->objects.insert(std::make_pair(new_ob->uid, new_ob));

								world_state->markAsChanged();
							}
						}

						break;
					}
				case Protocol::DestroyObject: // Client wants to destroy an object.
					{
						conPrint("DestroyObject");
						const UID object_uid = readUIDFromStream(msg_buffer);

						// If client is not logged in, refuse object modification.
						if(client_user.isNull())
						{
							writeErrorMessageToClient(socket, "You must be logged in to destroy an object.");
						}
						else
						{
							bool send_must_be_owner_msg = false;
							{
								Lock lock(world_state->mutex);
								auto res = cur_world_state->objects.find(object_uid);
								if(res != cur_world_state->objects.end())
								{
									WorldObject* ob = res->second.getPointer();

									// See if the user has permissions to alter this object:
									const bool have_delete_perms = userHasObjectWritePermissions(*ob, *client_user, this->connected_world_name, *cur_world_state);
									if(!have_delete_perms)
										send_must_be_owner_msg = true;
									else
									{
										// Mark object as dead
										ob->state = WorldObject::State_Dead;
										ob->from_remote_other_dirty = true;
										cur_world_state->addWorldObjectAsDBDirty(ob);
										cur_world_state->dirty_from_remote_objects.insert(ob);

										world_state->markAsChanged();
									}
								}
							} // End lock scope

							if(send_must_be_owner_msg)
								writeErrorMessageToClient(socket, "You must be the owner of this object to destroy it.");
						}
						break;
					}
				case Protocol::GetAllObjects: // Client wants to get all objects in world
				{
					conPrint("GetAllObjects");

					SocketBufferOutStream temp_buf(SocketBufferOutStream::DontUseNetworkByteOrder); // Will contain several messages

					{
						Lock lock(world_state->mutex);
						for(auto it = cur_world_state->objects.begin(); it != cur_world_state->objects.end(); ++it)
						{
							const WorldObject* ob = it->second.getPointer();

							// Build ObjectInitialSend message
							initPacket(scratch_packet, Protocol::ObjectInitialSend);
							ob->writeToNetworkStream(scratch_packet);
							updatePacketLengthField(scratch_packet);

							temp_buf.writeData(scratch_packet.buf.data(), scratch_packet.buf.size());
						}
					}

					initPacket(scratch_packet, Protocol::AllObjectsSent);// Terminate the buffer with an AllObjectsSent message.
					updatePacketLengthField(scratch_packet);
					temp_buf.writeData(scratch_packet.buf.data(), scratch_packet.buf.size());

					socket->writeData(temp_buf.buf.data(), temp_buf.buf.size());
					socket->flush();

					break;
				}
				case Protocol::QueryObjects: // Client wants to query objects in certain grid cells
				{
					const uint32 num_cells = msg_buffer.readUInt32();
					if(num_cells > 100000)
						throw glare::Exception("QueryObjects: too many cells: " + toString(num_cells));

					//conPrint("QueryObjects, num_cells=" + toString(num_cells));

					//conPrint("QueryObjects: num_cells " + toString(num_cells));
					
					// Read cell coords from network and make AABBs for cells
					js::Vector<js::AABBox, 16> cell_aabbs(num_cells);
					for(uint32 i=0; i<num_cells; ++i)
					{
						const int x = msg_buffer.readInt32();
						const int y = msg_buffer.readInt32();
						const int z = msg_buffer.readInt32();

						//if(i < 10)
						//	conPrint("cell " + toString(i) + " coords: " + toString(x) + ", " + toString(y) + ", " + toString(z));

						const float CELL_WIDTH = 200.f; // NOTE: has to be the same value as in gui_client/ProximityLoader.cpp.

						cell_aabbs[i] = js::AABBox(
							Vec4f(0,0,0,1) + Vec4f((float)x,     (float)y,     (float)z,     0)*CELL_WIDTH,
							Vec4f(0,0,0,1) + Vec4f((float)(x+1), (float)(y+1), (float)(z+1), 0)*CELL_WIDTH
						);
					}


					SocketBufferOutStream packet(SocketBufferOutStream::DontUseNetworkByteOrder);
					int num_obs_written = 0;

					{ // Lock scope
						Lock lock(world_state->mutex);
						for(auto it = cur_world_state->objects.begin(); it != cur_world_state->objects.end(); ++it)
						{
							const WorldObject* ob = it->second.ptr();

							// See if the object is in any of the cell AABBs
							bool in_cell = false;
							for(uint32 i=0; i<num_cells; ++i)
								if(cell_aabbs[i].contains(ob->pos.toVec4fPoint()))
								{
									in_cell = true;
									break;
								}

							if(in_cell)
							{
								// Send ObjectInitialSend packet
								initPacket(scratch_packet, Protocol::ObjectInitialSend);
								ob->writeToNetworkStream(scratch_packet);
								updatePacketLengthField(scratch_packet);

								packet.writeData(scratch_packet.buf.data(), scratch_packet.buf.size()); 

								num_obs_written++;
							}
						}
					} // End lock scope

					socket->writeData(packet.buf.data(), packet.buf.size()); // Write data to network
					socket->flush();

					//conPrint("Sent back info on " + toString(num_obs_written) + " object(s)");

					break;
				}
				case Protocol::QueryParcels:
					{
						conPrint("QueryParcels");
						// Send all current parcel data to client
						initPacket(scratch_packet, Protocol::ParcelList);
						{
							Lock lock(world_state->mutex);
							scratch_packet.writeUInt64(cur_world_state->parcels.size()); // Write num parcels
							for(auto it = cur_world_state->parcels.begin(); it != cur_world_state->parcels.end(); ++it)
								writeToNetworkStream(*it->second, scratch_packet); // Write parcel
						}
						updatePacketLengthField(scratch_packet);
						socket->writeData(scratch_packet.buf.data(), scratch_packet.buf.size()); // Send the data
						socket->flush();
						break;
					}
				case Protocol::ParcelFullUpdate: // Client wants to update a parcel
					{
						conPrint("ParcelFullUpdate");
						const ParcelID parcel_id = readParcelIDFromStream(msg_buffer);

						// Look up existing parcel in world state
						{
							bool read = false;

							// Only allow updating of parcels is this is a website connection.
							const bool have_permissions = false;// connection_type == Protocol::ConnectionTypeWebsite;

							if(have_permissions)
							{
								Lock lock(world_state->mutex);
								auto res = cur_world_state->parcels.find(parcel_id);
								if(res != cur_world_state->parcels.end())
								{
									// TODO: Check if this client has permissions to update the parcel information.

									Parcel* parcel = res->second.getPointer();
									readFromNetworkStreamGivenID(msg_buffer, *parcel);
									read = true;
									parcel->from_remote_dirty = true;
									cur_world_state->addParcelAsDBDirty(parcel);
								}
							}

							// Make sure we have read the whole pracel from the network stream
							if(!read)
							{
								Parcel dummy;
								readFromNetworkStreamGivenID(msg_buffer, dummy);
							}
						}
						break;
					}
				case Protocol::ChatMessageID:
					{
						//const std::string name = msg_buffer.readStringLengthFirst(MAX_STRING_LEN);
						const std::string msg = msg_buffer.readStringLengthFirst(MAX_STRING_LEN);

						conPrint("Received chat message: '" + msg + "'");

						if(client_user.isNull())
						{
							writeErrorMessageToClient(socket, "You must be logged in to chat.");
						}
						else
						{
							// Enqueue chat messages to worker threads to send
							// Send ChatMessageID packet
							initPacket(scratch_packet, Protocol::ChatMessageID);
							scratch_packet.writeStringLengthFirst(client_user->name);
							scratch_packet.writeStringLengthFirst(msg);
							updatePacketLengthField(scratch_packet);

							enqueuePacketToBroadcast(scratch_packet, server);
						}
						break;
					}
				case Protocol::UserSelectedObject:
					{
						//conPrint("Received UserSelectedObject msg.");

						const UID object_uid = readUIDFromStream(msg_buffer);

						// Send message to connected clients
						{
							initPacket(scratch_packet, Protocol::UserSelectedObject);
							writeToStream(client_avatar_uid, scratch_packet);
							writeToStream(object_uid, scratch_packet);
							updatePacketLengthField(scratch_packet);

							enqueuePacketToBroadcast(scratch_packet, server);
						}
						break;
					}
				case Protocol::UserDeselectedObject:
					{
						//conPrint("Received UserDeselectedObject msg.");

						const UID object_uid = readUIDFromStream(msg_buffer);

						// Send message to connected clients
						{
							initPacket(scratch_packet, Protocol::UserDeselectedObject);
							writeToStream(client_avatar_uid, scratch_packet);
							writeToStream(object_uid, scratch_packet);
							updatePacketLengthField(scratch_packet);

							enqueuePacketToBroadcast(scratch_packet, server);
						}
						break;
					}
				case Protocol::LogInMessage: // Client wants to log in.
					{
						conPrint("LogInMessage");

						const std::string username = msg_buffer.readStringLengthFirst(MAX_STRING_LEN);
						const std::string password = msg_buffer.readStringLengthFirst(MAX_STRING_LEN);

						conPrint("username: '" + username + "'");
						
						bool logged_in = false;
						{
							Lock lock(world_state->mutex);
							auto res = world_state->name_to_users.find(username);
							if(res != world_state->name_to_users.end())
							{
								User* user = res->second.getPointer();
								const bool password_valid = user->isPasswordValid(password);
								conPrint("password_valid: " + boolToString(password_valid));
								if(password_valid)
								{
									// Password is valid, log user in.
									client_user = user;

									logged_in = true;
								}
							}
						}

						conPrint("logged_in: " + boolToString(logged_in));
						if(logged_in)
						{
							if(username == "lightmapperbot")
								logged_in_user_is_lightmapper_bot = true;

							// Send logged-in message to client
							initPacket(scratch_packet, Protocol::LoggedInMessageID);
							writeToStream(client_user->id, scratch_packet);
							scratch_packet.writeStringLengthFirst(username);
							writeToStream(client_user->avatar_settings, scratch_packet);
							updatePacketLengthField(scratch_packet);

							socket->writeData(scratch_packet.buf.data(), scratch_packet.buf.size());
							socket->flush();
						}
						else
						{
							// Login failed.  Send error message back to client
							initPacket(scratch_packet, Protocol::ErrorMessageID);
							scratch_packet.writeStringLengthFirst("Login failed: username or password incorrect.");
							updatePacketLengthField(scratch_packet);

							socket->writeData(scratch_packet.buf.data(), scratch_packet.buf.size());
							socket->flush();
						}
					
						break;
					}
				case Protocol::LogOutMessage: // Client wants to log out.
					{
						conPrint("LogOutMessage");

						client_user = NULL; // Mark the client as not logged in.

						// Send logged-out message to client
						initPacket(scratch_packet, Protocol::LoggedOutMessageID);
						updatePacketLengthField(scratch_packet);

						socket->writeData(scratch_packet.buf.data(), scratch_packet.buf.size());
						socket->flush();
						break;
					}
				case Protocol::SignUpMessage:
					{
						conPrint("SignUpMessage");

						const std::string username = msg_buffer.readStringLengthFirst(MAX_STRING_LEN);
						const std::string email    = msg_buffer.readStringLengthFirst(MAX_STRING_LEN);
						const std::string password = msg_buffer.readStringLengthFirst(MAX_STRING_LEN);

						try
						{

							conPrint("username: '" + username + "', email: '" + email + "'");

							bool signed_up = false;

							std::string msg_to_client;
							if(username.size() < 3)
								msg_to_client = "Username is too short, must have at least 3 characters";
							else
							{
								if(password.size() < 6)
									msg_to_client = "Password is too short, must have at least 6 characters";
								else
								{
									Lock lock(world_state->mutex);
									auto res = world_state->name_to_users.find(username);
									if(res == world_state->name_to_users.end())
									{
										Reference<User> new_user = new User();
										new_user->id = UserID((uint32)world_state->name_to_users.size());
										new_user->created_time = TimeStamp::currentTime();
										new_user->name = username;
										new_user->email_address = email;

										// We need a random salt for the user.
										uint8 random_bytes[32];
										CryptoRNG::getRandomBytes(random_bytes, 32); // throws glare::Exception

										std::string user_salt;
										Base64::encode(random_bytes, 32, user_salt); // Convert random bytes to base-64.

										new_user->password_hash_salt = user_salt;
										new_user->hashed_password = User::computePasswordHash(password, user_salt);

										world_state->addUserAsDBDirty(new_user);

										// Add new user to world state
										world_state->user_id_to_users.insert(std::make_pair(new_user->id, new_user));
										world_state->name_to_users   .insert(std::make_pair(username,     new_user));
										world_state->markAsChanged(); // Mark as changed so gets saved to disk.

										client_user = new_user; // Log user in as well.
										signed_up = true;
									}
								}
							}

							conPrint("signed_up: " + boolToString(signed_up));
							if(signed_up)
							{
								conPrint("Sign up successful");
								// Send signed-up message to client
								initPacket(scratch_packet, Protocol::SignedUpMessageID);
								writeToStream(client_user->id, scratch_packet);
								scratch_packet.writeStringLengthFirst(username);
								updatePacketLengthField(scratch_packet);

								socket->writeData(scratch_packet.buf.data(), scratch_packet.buf.size());
								socket->flush();
							}
							else
							{
								conPrint("Sign up failed.");

								// signup failed.  Send error message back to client
								initPacket(scratch_packet, Protocol::ErrorMessageID);
								scratch_packet.writeStringLengthFirst(msg_to_client);
								updatePacketLengthField(scratch_packet);

								socket->writeData(scratch_packet.buf.data(), scratch_packet.buf.size());
								socket->flush();
							}
						}
						catch(glare::Exception& e)
						{
							conPrint("Sign up failed, internal error: " + e.what());

							// signup failed.  Send error message back to client
							initPacket(scratch_packet, Protocol::ErrorMessageID);
							scratch_packet.writeStringLengthFirst("Signup failed: internal error.");
							updatePacketLengthField(scratch_packet);

							socket->writeData(scratch_packet.buf.data(), scratch_packet.buf.size());
							socket->flush();
						}

						break;
					}
				case Protocol::RequestPasswordReset:
					{
						conPrint("RequestPasswordReset");

						const std::string email    = msg_buffer.readStringLengthFirst(MAX_STRING_LEN);

						// NOTE: This stuff is done via the website now instead.

						//conPrint("email: " + email);
						//
						//// TEMP: Send password reset email in this thread for now. 
						//// TODO: move to another thread (make some kind of background task?)
						//{
						//	Lock lock(world_state->mutex);
						//	for(auto it = world_state->user_id_to_users.begin(); it != world_state->user_id_to_users.end(); ++it)
						//		if(it->second->email_address == email)
						//		{
						//			User* user = it->second.getPointer();
						//			try
						//			{
						//				user->sendPasswordResetEmail();
						//				world_state->markAsChanged(); // Mark as changed so gets saved to disk.
						//				conPrint("Sent user password reset email to '" + email + ", username '" + user->name + "'");
						//			}
						//			catch(glare::Exception& e)
						//			{
						//				conPrint("Sending password reset email failed: " + e.what());
						//			}
						//		}
						//}
					
						break;
					}
				case Protocol::ChangePasswordWithResetToken:
					{
						conPrint("ChangePasswordWithResetToken");
						
						const std::string email			= msg_buffer.readStringLengthFirst(MAX_STRING_LEN);
						const std::string reset_token	= msg_buffer.readStringLengthFirst(MAX_STRING_LEN);
						const std::string new_password	= msg_buffer.readStringLengthFirst(MAX_STRING_LEN);

						// NOTE: This stuff is done via the website now instead.
						// 
						//conPrint("email: " + email);
						//conPrint("reset_token: " + reset_token);
						////conPrint("new_password: " + new_password);
						//
						//{
						//	Lock lock(world_state->mutex);
						//
						//	// Find user with the given email address:
						//	for(auto it = world_state->user_id_to_users.begin(); it != world_state->user_id_to_users.end(); ++it)
						//		if(it->second->email_address == email)
						//		{
						//			User* user = it->second.getPointer();
						//			const bool reset = user->resetPasswordWithToken(reset_token, new_password);
						//			if(reset)
						//			{
						//				world_state->markAsChanged(); // Mark as changed so gets saved to disk.
						//				conPrint("User password successfully updated.");
						//			}
						//		}
						//}

						break;
					}
				default:			
					{
						//conPrint("Unknown message id: " + toString(msg_type));
						throw glare::Exception("Unknown message id: " + toString(msg_type));
					}
				}
			}
			else
			{
#if defined(_WIN32) || defined(OSX)
#else
				if(VERBOSE) conPrint("WorkerThread: event FD was signalled.");

				// The event FD was signalled, which means there is some data to send on the socket.
				// Reset the event fd by reading from it.
				event_fd.read();

				if(VERBOSE) conPrint("WorkerThread: event FD has been reset.");
#endif
			}
		}
	}
	catch(MySocketExcep& e)
	{
		if(e.excepType() == MySocketExcep::ExcepType_ConnectionClosedGracefully)
			conPrint("Updates client from " + IPAddress::formatIPAddressAndPort(socket->getOtherEndIPAddress(), socket->getOtherEndPort()) + " closed connection gracefully.");
		else
			conPrint("Socket error: " + e.what());
	}
	catch(glare::Exception& e)
	{
		conPrint("glare::Exception: " + e.what());
	}
	catch(std::bad_alloc&)
	{
		conPrint("WorkerThread: Caught std::bad_alloc.");
	}

	// Mark avatar corresponding to client as dead
	if(cur_world_state.nonNull())
	{
		Lock lock(world_state->mutex);
		if(cur_world_state->avatars.count(client_avatar_uid) == 1)
		{
			cur_world_state->avatars[client_avatar_uid]->state = Avatar::State_Dead;
			cur_world_state->avatars[client_avatar_uid]->other_dirty = true;
		}
	}
}


void WorkerThread::enqueueDataToSend(const std::string& data)
{
	if(VERBOSE) conPrint("WorkerThread::enqueueDataToSend(), data: '" + data + "'");

	// Append data to data_to_send
	if(!data.empty())
	{
		Lock lock(data_to_send_mutex);
		const size_t write_i = data_to_send.size();
		data_to_send.resize(write_i + data.size());
		std::memcpy(&data_to_send[write_i], data.data(), data.size());
	}

	event_fd.notify();
}


void WorkerThread::enqueueDataToSend(const SocketBufferOutStream& packet) // threadsafe
{
	// Append data to data_to_send
	if(!packet.buf.empty())
	{
		Lock lock(data_to_send_mutex);
		const size_t write_i = data_to_send.size();
		data_to_send.resize(write_i + packet.buf.size());
		std::memcpy(&data_to_send[write_i], packet.buf.data(), packet.buf.size());
	}

	event_fd.notify();
}