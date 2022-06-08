/*=====================================================================
ModelLoading.h
--------------
Copyright Glare Technologies Limited 2016 -
=====================================================================*/
#pragma once


#include "../shared/WorldMaterial.h"
#include "../shared/WorldObject.h"
#include <opengl/OpenGLEngine.h>
#include <dll/include/IndigoMesh.h>
#include <graphics/BatchedMesh.h>
#include <utils/ManagerWithCache.h>


struct GLObject;
class Matrix4f;
class ResourceManager;
class RayMesh;
class VoxelGroup;
class VertexBufferAllocator;
namespace Indigo { class TaskManager; }


/*=====================================================================
ModelLoading
-------------

=====================================================================*/
class ModelLoading
{
public:
	static void setGLMaterialFromWorldMaterialWithLocalPaths(const WorldMaterial& mat, OpenGLMaterial& opengl_mat);
	static void setGLMaterialFromWorldMaterial(const WorldMaterial& mat, int lod_level, const std::string& lightmap_url, ResourceManager& resource_manager, OpenGLMaterial& opengl_mat);


	static void checkValidAndSanitiseMesh(Indigo::Mesh& mesh);


	static bool hasSupportedModelExtension(const std::string& path);

	// Load a mesh from disk.
	// Make an OpenGL object from it, also make an IndigoMesh from it (unless we are loading a voxel format such as .vox).
	// If loading a voxel format, set loaded_object_out.voxel_group.
	// Set loaded_object_out.materials as well.
	// May set a scale on loaded_object_out.
	//
	// Throws glare::Exception on invalid mesh.
	static GLObjectRef makeGLObjectForModelFile(OpenGLEngine& gl_engine, VertexBufferAllocator& vert_buf_allocator, glare::TaskManager& task_manager, const std::string& path,
		BatchedMeshRef& mesh_out,
		WorldObject& loaded_object_out);


	// This is a cube object with two materials, for displaying images or videos.
	// Sets loaded_object_in_out scale and materials
	static GLObjectRef makeImageCube(OpenGLEngine& gl_engine, VertexBufferAllocator& vert_buf_allocator, glare::TaskManager& task_manager, 
		const std::string& image_path, int im_w, int im_h,
		BatchedMeshRef& mesh_out,
		WorldObject& loaded_object_in_out);


	static GLObjectRef makeGLObjectForMeshDataAndMaterials(OpenGLEngine& gl_engine, const Reference<OpenGLMeshRenderData> gl_meshdata, /*size_t num_materials_referenced, */int ob_lod_level, const std::vector<WorldMaterialRef>& materials, 
		const std::string& lightmap_url, ResourceManager& resource_manager, const Matrix4f& ob_to_world_matrix);

	static void setMaterialTexPathsForLODLevel(GLObject& gl_ob, int ob_lod_level, const std::vector<WorldMaterialRef>& materials,
		const std::string& lightmap_url, ResourceManager& resource_manager);


	static Reference<OpenGLMeshRenderData> makeGLMeshDataAndRayMeshForModelURL(const std::string& lod_model_URL,
		ResourceManager& resource_manager, glare::TaskManager& task_manager, VertexBufferAllocator* vert_buf_allocator,
		bool skip_opengl_calls, Reference<RayMesh>& raymesh_out);


	static Reference<OpenGLMeshRenderData> makeModelForVoxelGroup(const VoxelGroup& voxel_group, int subsample_factor, const Matrix4f& ob_to_world, 
		glare::TaskManager& task_manager, VertexBufferAllocator* vert_buf_allocator, bool do_opengl_stuff, Reference<RayMesh>& raymesh_out);

	//static Reference<BatchedMesh> makeBatchedMeshForVoxelGroup(const VoxelGroup& voxel_group);
	//static Reference<Indigo::Mesh> makeIndigoMeshForVoxelGroup(const VoxelGroup& voxel_group);

	static void test();
};

