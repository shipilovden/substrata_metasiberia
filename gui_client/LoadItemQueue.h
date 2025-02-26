/*=====================================================================
LoadItemQueue.h
---------------
Copyright Glare Technologies Limited 2022 -
=====================================================================*/
#pragma once


#include <Platform.h>
#include <Vector.h>
#include <Task.h>
#include <physics/jscol_aabbox.h>
#include <maths/Vec4.h>
#include <maths/vec3.h>
#include <vector>
class WorldObject;
class Avatar;


struct LoadItemQueueItem
{
	GLARE_ALIGNED_16_NEW_DELETE

	static float sizeFactorForAABBWS(float aabb_ws_longest_len, float importance_factor)
	{
		// object projected angle    theta ~= aabb_ws.longestLength() / ob_dist
		
		// We will sort in ascending order by 1 / theta so that objects with a larger projected angle are loaded first.

		// 1 / theta = 1 / (aabb_ws.longestLength() / ob_dist) = ob_dist / aabb_ws.longestLength()

		const float min_len = 1.f; // Objects smaller than 1 m are considered just as important as 1 m wide objects.

		return 1.f / (myMax(min_len, aabb_ws_longest_len) * importance_factor);
	}

	Vec4f pos;
	float size_factor;
	float task_max_dist; // Max distance from camera before task should be discarded.
	glare::TaskRef task;
};


/*=====================================================================
LoadItemQueue
-------------
Queue of load model tasks, load texture tasks etc, together with the position of the item,
which is used for sorting the tasks based on distance from the camera.
=====================================================================*/
class LoadItemQueue
{
public:
	LoadItemQueue();
	~LoadItemQueue();

	void enqueueItem(const WorldObject& ob, const glare::TaskRef& task, float task_max_dist);
	void enqueueItem(const Avatar& ob, const glare::TaskRef& task, float task_max_dist, bool our_avatar);
	void enqueueItem(const Vec4f& pos, float aabb_ws_longest_len, const glare::TaskRef& task, float task_max_dist, float importance_factor);
	void enqueueItem(const Vec4f& pos, float size_factor, const glare::TaskRef& task, float task_max_dist);

	void clear();

	bool empty() const { return size() == 0; }

	size_t size() const;

	void sortQueue(const Vec3d& campos); // Sort queue (by item distance to camera)

	LoadItemQueueItem dequeueFront();
private:
	// Valid items are at indices [begin_i, items.size())
	size_t begin_i;
	js::Vector<LoadItemQueueItem, 16> items;
};
