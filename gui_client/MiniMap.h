/*=====================================================================
MiniMap.h
---------
Copyright Glare Technologies Limited 2023 -
=====================================================================*/
#pragma once


#include <opengl/ui/GLUI.h>
#include <opengl/ui/GLUIButton.h>
#include <opengl/ui/GLUICallbackHandler.h>
#include <opengl/ui/GLUITextView.h>
#include <opengl/ui/GLUIImage.h>
#include <maths/vec3.h>
#include <utils/Array2D.h>
#include <map>


class MainWindow;
class Avatar;
class OpenGLScene;
class MapTilesResultReceivedMessage;


struct MapTile
{
	Reference<OverlayObject> ob;
};


struct MapTileInfo
{
	std::string image_URL;
};


/*=====================================================================
MiniMap
-------

=====================================================================*/
class MiniMap : public GLUICallbackHandler
{
public:
	MiniMap();
	~MiniMap();

	void create(Reference<OpenGLEngine>& opengl_engine_, MainWindow* main_window_, GLUIRef gl_ui_);
	void destroy();

	void think();

	void viewportResized(int w, int h);

	//virtual bool doHandleMouseMoved(const Vec2f& coords) override;

	// GLUICallbackHandler interface:
	void eventOccurred(GLUICallbackEvent& event) override;
	void mouseWheelEventOccurred(GLUICallbackMouseWheelEvent& event) override;

	void handleMapTilesResultReceivedMessage(const MapTilesResultReceivedMessage& msg);
private:
	void checkUpdateTilesForCurCamPosition();
	void updateWidgetPositions();
	void renderTilesToTexture();

	MainWindow* main_window;
	GLUIRef gl_ui;
	Reference<OpenGLEngine> opengl_engine;

	GLUIImageRef minimap_image;

	Array2D<MapTile> tiles;
	int last_centre_x, last_centre_y;

	std::map<Vec3i, MapTileInfo> tile_infos;

	Reference<FrameBuffer> frame_buffer;

	Reference<OpenGLTexture> minimap_texture;

	Reference<OpenGLScene> scene;

	Vec3d last_requested_campos;
	int last_requested_tile_z;

	float map_width_ws;
};
