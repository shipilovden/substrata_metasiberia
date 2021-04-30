/*=====================================================================
WebServerRequestHandler.cpp
---------------------------
Copyright Glare Technologies Limited 2021 -
=====================================================================*/
#include "WebServerRequestHandler.h"


#include "WebDataStore.h"
#include "AdminHandlers.h"
#include "MainPageHandlers.h"
#include "WebsiteExcep.h"
#include "Escaping.h"
#include "LoginHandlers.h"
#include "ResponseUtils.h"
#include "RequestHandler.h"
#include "PayPalHandlers.h"
#include "CoinbaseHandlers.h"
#include "AuctionHandlers.h"
#include "ScreenshotHandlers.h"
#include "OrderHandlers.h"
#include "ParcelHandlers.h"
#include "ResourceHandlers.h"
#include <StringUtils.h>
#include <Parser.h>
#include <MemMappedFile.h>
#include <ConPrint.h>
#include <FileUtils.h>
#include <Exception.h>
#include <Lock.h>


WebServerRequestHandler::WebServerRequestHandler()
{}


WebServerRequestHandler::~WebServerRequestHandler()
{}


static bool isLetsEncryptFileQuerySafe(const std::string& s)
{
	for(size_t i=0; i<s.size(); ++i)
		if(!(::isAlphaNumeric(s[i]) || (s[i] == '-') || (s[i] == '_') || (s[i] == '.')))
			return false;
	return true;
}


static bool isFileNameSafe(const std::string& s)
{
	for(size_t i=0; i<s.size(); ++i)
		if(!(::isAlphaNumeric(s[i]) || (s[i] == '-') || (s[i] == '_') || (s[i] == '.')))
			return false;
	return true;
}


void WebServerRequestHandler::handleRequest(const web::RequestInfo& request, web::ReplyInfo& reply_info)
{
	if(!request.tls_connection)
	{
		// Redirect to https (unless the server is running on localhost, which we will allow to use non-https for testing)

		// Find the hostname the request was sent to - look through the headers for 'host'.
		std::string hostname;
		for(size_t i=0; i<request.headers.size(); ++i)
			if(StringUtils::equalCaseInsensitive(request.headers[i].key, "host"))
				hostname = request.headers[i].value.to_string();
		if(hostname != "localhost")
		{
			const std::string response = 
				"HTTP/1.1 301 Redirect\r\n" // 301 = Moved Permanently
				"Location: https://" + hostname + request.path + "\r\n"
				"Content-Length: 0\r\n"
				"\r\n";
			reply_info.socket->writeData(response.c_str(), response.size());
			return;
		}
	}


	if(request.verb == "POST")
	{
		// Route PUT request
		if(request.path == "/login_post")
		{
			LoginHandlers::handleLoginPost(*this->world_state, request, reply_info);
		}
		else if(request.path == "/logout_post")
		{
			LoginHandlers::handleLogoutPost(request, reply_info);
		}
		else if(request.path == "/signup_post")
		{
			LoginHandlers::handleSignUpPost(*this->world_state, request, reply_info);
		}
		else if(request.path == "/ipn_listener")
		{
			PayPalHandlers::handleIPNPost(*this->world_state, request, reply_info);
		}
		else if(request.path == "/coinbase_webhook")
		{
			CoinbaseHandlers::handleCoinbaseWebhookPost(*this->world_state, request, reply_info);
		}
		else if(request.path == "/buy_parcel_now_paypal")
		{
			AuctionHandlers::handleParcelBuyNowWithPayPal(*this->world_state, request, reply_info);
		}
		else if(request.path == "/buy_parcel_now_coinbase")
		{
			AuctionHandlers::handleParcelBuyNowWithCoinbase(*this->world_state, request, reply_info);
		}
		else if(request.path == "/buy_parcel_with_paypal_post")
		{
			AuctionHandlers::handleBuyParcelWithPayPalPost(*this->world_state, request, reply_info);
		}
		else if(request.path == "/buy_parcel_with_coinbase_post")
		{
			AuctionHandlers::handleBuyParcelWithCoinbasePost(*this->world_state, request, reply_info);
		}
		else if(request.path == "/admin_create_parcel_auction_post")
		{
			AdminHandlers::createParcelAuctionPost(*this->world_state, request, reply_info);
		}
		else if(request.path == "/admin_set_parcel_owner_post")
		{
			AdminHandlers::handleSetParcelOwnerPost(*this->world_state, request, reply_info);
		}
		else if(request.path == "/admin_regenerate_parcel_auction_screenshots")
		{
			AdminHandlers::handleRegenerateParcelAuctionScreenshots(*this->world_state, request, reply_info);
		}
		else if(request.path == "/admin_terminate_parcel_auction")
		{
			AdminHandlers::handleTerminateParcelAuction(*this->world_state, request, reply_info);
		}
		else
		{
			const std::string page = "Unknown post URL";
			web::ResponseUtils::writeHTTPOKHeaderAndData(reply_info, page);
		}
	}
	else if(request.verb == "GET")
	{
		// Route GET request
		if(request.path == "/")
		{
			MainPageHandlers::renderRootPage(*this->world_state, request, reply_info);
		}
		else if(request.path == "/terms")
		{
			MainPageHandlers::renderTermsOfUse(*this->world_state, request, reply_info);
		}
		else if(request.path == "/about_parcel_sales")
		{
			MainPageHandlers::renderAboutParcelSales(*this->world_state, request, reply_info);
		}
		else if(request.path == "/about_scripting")
		{
			MainPageHandlers::renderAboutScripting(*this->world_state, request, reply_info);
		}
		else if(request.path == "/pdt_landing")
		{
			PayPalHandlers::handlePayPalPDTOrderLanding(*this->world_state, request, reply_info);
		}
		else if(request.path == "/parcel_auction_list")
		{
			AuctionHandlers::renderParcelAuctionListPage(*this->world_state, request, reply_info);
		}
		else if(::hasPrefix(request.path, "/parcel_auction/")) // parcel auction ID follows in URL
		{
			AuctionHandlers::renderParcelAuctionPage(*this->world_state, request, reply_info);
		}
		else if(::hasPrefix(request.path, "/buy_parcel_with_paypal/")) // parcel ID follows in URL
		{
			AuctionHandlers::renderBuyParcelWithPayPalPage(*this->world_state, request, reply_info);
		}
		else if(::hasPrefix(request.path, "/buy_parcel_with_coinbase/")) // parcel ID follows in URL
		{
			AuctionHandlers::renderBuyParcelWithCoinbasePage(*this->world_state, request, reply_info);
		}
		else if(::hasPrefix(request.path, "/order/")) // Order ID follows in URL
		{
			OrderHandlers::renderOrderPage(*this->world_state, request, reply_info);
		}
		else if(::hasPrefix(request.path, "/parcel/")) // Parcel ID follows in URL
		{
			ParcelHandlers::renderParcelPage(*this->world_state, request, reply_info);
		}
		else if(request.path == "/admin")
		{
			AdminHandlers::renderMainAdminPage(*this->world_state, request, reply_info);
		}
		else if(request.path == "/admin_users")
		{
			AdminHandlers::renderUsersPage(*this->world_state, request, reply_info);
		}
		else if(request.path == "/admin_parcels")
		{
			AdminHandlers::renderParcelsPage(*this->world_state, request, reply_info);
		}
		else if(request.path == "/admin_parcel_auctions")
		{
			AdminHandlers::renderParcelAuctionsPage(*this->world_state, request, reply_info);
		}
		else if(request.path == "/admin_orders")
		{
			AdminHandlers::renderOrdersPage(*this->world_state, request, reply_info);
		}
		else if(::hasPrefix(request.path, "/admin_create_parcel_auction/")) // parcel ID follows in URL
		{
			AdminHandlers::renderCreateParcelAuction(*this->world_state, request, reply_info);
		}
		else if(::hasPrefix(request.path, "/admin_set_parcel_owner/")) // parcel ID follows in URL
		{
			AdminHandlers::renderSetParcelOwnerPage(*this->world_state, request, reply_info);
		}
		else if(::hasPrefix(request.path, "/admin_order/")) // order ID follows in URL
		{
			AdminHandlers::renderAdminOrderPage(*this->world_state, request, reply_info);
		}
		else if(request.path == "/login")
		{
			LoginHandlers::renderLoginPage(request, reply_info);
		}
		else if(request.path == "/signup")
		{
			LoginHandlers::renderSignUpPage(request, reply_info);
		}
		else if(::hasPrefix(request.path, "/screenshot/")) // Screenshot ID follows
		{
			ScreenshotHandlers::handleScreenshotRequest(*world_state, *data_store, request, reply_info);
		}
		else if(::hasPrefix(request.path, "/files/"))
		{
			const std::string filename = ::eatPrefix(request.path, "/files/");
			if(isFileNameSafe(filename))
			{
				try
				{
					MemMappedFile file(data_store->public_files_dir + "/" + filename);
					
					const std::string content_type = web::ResponseUtils::getContentTypeForPath(filename);

					web::ResponseUtils::writeHTTPOKHeaderAndData(reply_info, file.fileData(), file.fileSize(), content_type.c_str());
				}
				catch(glare::Exception&)
				{
					web::ResponseUtils::writeHTTPOKHeaderAndData(reply_info, "Failed to load file '" + filename + "'.");
				}
			}
			else
			{
				web::ResponseUtils::writeHTTPOKHeaderAndData(reply_info, "invalid/unsafe filename");
				return;
			}
		}
		else if(::hasPrefix(request.path, "/.well-known/acme-challenge/")) // Support for Let's encrypt: Serve up challenge response file.
		{
			const std::string filename = ::eatPrefix(request.path, "/.well-known/acme-challenge/");
			if(!isLetsEncryptFileQuerySafe(filename))
			{
				web::ResponseUtils::writeHTTPOKHeaderAndData(reply_info, "invalid/unsafe file query");
				return;
			}

			// Serve up the file
			try
			{
				std::string contents;
				FileUtils::readEntireFile(data_store->letsencrypt_webroot + "/.well-known/acme-challenge/" + filename, contents);
				web::ResponseUtils::writeHTTPOKHeaderAndData(reply_info, contents);
			}
			catch(FileUtils::FileUtilsExcep& e)
			{
				web::ResponseUtils::writeHTTPOKHeaderAndData(reply_info, "Failed to load file '" + filename + "': " + e.what());
			}
		}
		else if(::hasPrefix(request.path, "/resource/"))
		{
			ResourceHandlers::handleResourceRequest(*this->world_state, request, reply_info);
		}
		else
		{
			std::string page = "Unknown page";
			web::ResponseUtils::writeHTTPNotFoundHeaderAndData(reply_info, page);
		}
	}
}


void WebServerRequestHandler::handleWebsocketTextMessage(const std::string& msg, Reference<SocketInterface>& socket, const Reference<WorkerThread>& worker_thread)
{}


void WebServerRequestHandler::websocketConnectionClosed(Reference<SocketInterface>& socket, const Reference<WorkerThread>& worker_thread)
{}
