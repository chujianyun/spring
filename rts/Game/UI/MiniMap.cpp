/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include <SDL_keycode.h>
#include <SDL_mouse.h>

#include "CommandColors.h"
#include "GuiHandler.h"
#include "MiniMap.h"
#include "MouseHandler.h"
#include "TooltipConsole.h"
#include "Game/Camera.h"
#include "Game/CameraHandler.h"
#include "Game/GameHelper.h"
#include "Game/GlobalUnsynced.h"
#include "Game/SelectedUnitsHandler.h"
#include "Game/Players/Player.h"
#include "Game/UI/UnitTracker.h"
#include "Sim/Misc/TeamHandler.h"
#include "Lua/LuaUnsyncedCtrl.h"
#include "Map/BaseGroundDrawer.h"
#include "Map/Ground.h"
#include "Map/ReadMap.h"
#include "Rendering/CommandDrawer.h"
#include "Rendering/IconHandler.h"
#include "Rendering/LineDrawer.h"
#include "Rendering/Env/Particles/ProjectileDrawer.h"
#include "Rendering/UnitDrawer.h"
#include "Rendering/GL/myGL.h"
#include "Rendering/GL/glExtra.h"
#include "Rendering/GL/VertexArray.h"
#include "Rendering/GL/RenderDataBuffer.hpp"
#include "Rendering/Textures/Bitmap.h"
#include "Sim/Units/CommandAI/CommandAI.h"
#include "Sim/Units/Unit.h"
#include "Sim/Units/UnitHandler.h"
#include "Sim/Weapons/WeaponDefHandler.h"
#include "Sim/Weapons/Weapon.h"
#include "System/Config/ConfigHandler.h"
#include "System/EventHandler.h"
#include "System/StringHash.h"
#include "System/StringUtil.h"
#include "System/TimeProfiler.h"
#include "System/Input/KeyInput.h"
#include "System/FileSystem/SimpleParser.h"
#include "System/Sound/ISoundChannels.h"

CONFIG(std::string, MiniMapGeometry).defaultValue("2 2 200 200");
CONFIG(bool, MiniMapFullProxy).defaultValue(true);
CONFIG(int, MiniMapButtonSize).defaultValue(16);

CONFIG(float, MiniMapUnitSize)
	.defaultValue(2.5f)
	.minimumValue(0.0f);

CONFIG(float, MiniMapUnitExp).defaultValue(0.25f);
CONFIG(float, MiniMapCursorScale).defaultValue(-0.5f);
CONFIG(bool, MiniMapIcons).defaultValue(true).headlessValue(false);

CONFIG(int, MiniMapDrawCommands).defaultValue(1).headlessValue(0).minimumValue(0);

CONFIG(bool, MiniMapDrawProjectiles).defaultValue(true).headlessValue(false);
CONFIG(bool, SimpleMiniMapColors).defaultValue(false);

CONFIG(int, MiniMapRefreshRate).defaultValue(0).minimumValue(0).description("Refresh rate of asynchronous MiniMap texture updates. Value of \"0\" autoselects between 10-60FPS.");



//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////


CMiniMap* minimap = nullptr;

CMiniMap::CMiniMap(): CInputReceiver(BACK)
{
	lastWindowSizeX = globalRendering->viewSizeX;
	lastWindowSizeY = globalRendering->viewSizeY;

	if (globalRendering->dualScreenMode) {
		curDim.x = globalRendering->viewSizeX;
		curDim.y = globalRendering->viewSizeY;
		curPos.x = (globalRendering->viewSizeX - globalRendering->viewPosX);
		curPos.y = 0;
	} else {
		ParseGeometry(configHandler->GetString("MiniMapGeometry"));
	}

	{
		fullProxy = configHandler->GetBool("MiniMapFullProxy");
		buttonSize = configHandler->GetInt("MiniMapButtonSize");

		unitBaseSize = configHandler->GetFloat("MiniMapUnitSize");
		unitExponent = configHandler->GetFloat("MiniMapUnitExp");

		cursorScale = configHandler->GetFloat("MiniMapCursorScale");
		useIcons = configHandler->GetBool("MiniMapIcons");
		drawCommands = configHandler->GetInt("MiniMapDrawCommands");
		drawProjectiles = configHandler->GetBool("MiniMapDrawProjectiles");
		simpleColors = configHandler->GetBool("SimpleMiniMapColors");
		minimapRefreshRate = configHandler->GetInt("MiniMapRefreshRate");
	}

	UpdateGeometry();


	// setup the buttons' texture and texture coordinates
	CBitmap bitmap;
	bool unfiltered = false;

	if (bitmap.Load("bitmaps/minimapbuttons.png")) {
		glGenTextures(1, &buttonsTextureID);
		glBindTexture(GL_TEXTURE_2D, buttonsTextureID);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, bitmap.xsize, bitmap.ysize, 0, GL_RGBA, GL_UNSIGNED_BYTE, bitmap.GetRawMem());
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);

		if ((unfiltered = (bitmap.ysize == buttonSize && bitmap.xsize == (buttonSize * 4)))) {
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		} else {
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		}

		glBindTexture(GL_TEXTURE_2D, 0);

		resizeBox.color = {1.0f, 1.0f, 1.0f, 1.0f};
		moveBox.color = {1.0f, 1.0f, 1.0f, 1.0f};
		maximizeBox.color = {1.0f, 1.0f, 1.0f, 1.0f};
		minimizeBox.color = {1.0f, 1.0f, 1.0f, 1.0f};

	} else {
		bitmap.AllocDummy({255, 255, 255, 255});
		buttonsTextureID = bitmap.CreateTexture();

		resizeBox.color = {0.1f, 0.1f, 0.8f, 0.8f}; // blue
		moveBox.color = {0.0f, 0.8f, 0.0f, 0.8f}; // green
		maximizeBox.color = {0.8f, 0.8f, 0.0f, 0.8f}; // yellow
		minimizeBox.color = {0.8f, 0.0f, 0.0f, 0.8f}; // red
	}

	{
		const float xshift = unfiltered ? 0.0f : (0.5f / bitmap.xsize);
		const float yshift = unfiltered ? 0.0f : (0.5f / bitmap.ysize);

			moveBox.xminTx = 0.50f + xshift;
			moveBox.xmaxTx = 0.75f - xshift;
		  resizeBox.xminTx = 0.75f + xshift;
		  resizeBox.xmaxTx = 1.00f - xshift;
		minimizeBox.xminTx = 0.00f + xshift;
		minimizeBox.xmaxTx = 0.25f - xshift;
		maximizeBox.xminTx = 0.25f + xshift;
		maximizeBox.xmaxTx = 0.50f - xshift;
			moveBox.yminTx = 1.00f - yshift;
		  resizeBox.yminTx = 1.00f - yshift;
		minimizeBox.yminTx = 1.00f - yshift;
		maximizeBox.yminTx = 1.00f - yshift;
			moveBox.ymaxTx = 0.00f + yshift;
		  resizeBox.ymaxTx = 0.00f + yshift;
		minimizeBox.ymaxTx = 0.00f + yshift;
		maximizeBox.ymaxTx = 0.00f + yshift;
	}
}


CMiniMap::~CMiniMap()
{
	glDeleteTextures(1, &buttonsTextureID);
	glDeleteTextures(1, &minimapTextureID);
}


void CMiniMap::ParseGeometry(const string& geostr)
{
	const std::string geodef = "2 2 200 200";

	if ((sscanf(geostr.c_str(), "%i %i %i %i", &curPos.x, &curPos.y, &curDim.x, &curDim.y) == 4) && (geostr == geodef)) {
		// default geometry
		curDim.x = -200;
		curDim.y = -200;
	} else {
		if (curDim.x <= 0) { curDim.x = -200; }
		if (curDim.y <= 0) { curDim.y = -200; }
	}

	if ((curDim.x <= 0) && (curDim.y <= 0)) {
		const float hw = math::sqrt(mapDims.mapx * 1.0f / mapDims.mapy);
		curDim.x = (-curDim.x * hw);
		curDim.y = (-curDim.y / hw);
	}
	else if (curDim.x <= 0) {
		curDim.x = curDim.y * (mapDims.mapx * 1.0f / mapDims.mapy);
	}
	else if (curDim.y <= 0) {
		curDim.y = curDim.x * (mapDims.mapy * 1.0f / mapDims.mapx);
	}

	// convert to GL coords (MM origin in bottom-left corner)
	curPos.y = globalRendering->viewSizeY - curDim.y - curPos.y;
}


void CMiniMap::ToggleMaximized(bool _maxspect)
{
	if ((maximized = !maximized)) {
		// stash current geometry
		oldPos = curPos;
		oldDim = curDim;
		maxspect = _maxspect;
	} else {
		// restore previous geometry
		curPos = oldPos;
		curDim = oldDim;
	}

	// needed for SetMaximizedGeometry
	UpdateGeometry();
}


void CMiniMap::SetMaximizedGeometry()
{
	if (!maxspect) {
		curDim.y = globalRendering->viewSizeY;
		curDim.x = curDim.y;
		curPos.x = (globalRendering->viewSizeX - globalRendering->viewSizeY) / 2;
		curPos.y = 0;
		return;
	}

	const float  mapRatio = (float)mapDims.mapx / (float)mapDims.mapy;
	const float viewRatio = globalRendering->aspectRatio;

	if (mapRatio > viewRatio) {
		curPos.x = 0;
		curDim.x = globalRendering->viewSizeX;
		curDim.y = globalRendering->viewSizeX / mapRatio;
		curPos.y = (globalRendering->viewSizeY - curDim.y) / 2;
	} else {
		curPos.y = 0;
		curDim.y = globalRendering->viewSizeY;
		curDim.x = globalRendering->viewSizeY * mapRatio;
		curPos.x = (globalRendering->viewSizeX - curDim.x) / 2;
	}
}


/******************************************************************************/

void CMiniMap::SetSlaveMode(bool newMode)
{
	if (newMode) {
		proxyMode   = false;
		selecting   = false;
		maxspect    = false;
		maximized   = false;
		minimized   = false;
		mouseLook   = false;
		mouseMove   = false;
		mouseResize = false;
	}

	if (newMode != slaveDrawMode) {
		static int oldButtonSize = 16;

		if (newMode) {
			oldButtonSize = buttonSize;
			buttonSize = 0;
		} else {
			buttonSize = oldButtonSize;
		}
	}

	slaveDrawMode = newMode;
	UpdateGeometry();
}


void CMiniMap::ConfigCommand(const std::string& line)
{
	const std::vector<std::string>& words = CSimpleParser::Tokenize(line, 1);
	if (words.empty())
		return;

	const std::string command = StringToLower(words[0]);

	switch (hashString(command.c_str())) {
		case hashString("fullproxy"): {
			fullProxy = (words.size() >= 2) ? !!atoi(words[1].c_str()) : !fullProxy;
		} break;
		case hashString("icons"): {
			useIcons = (words.size() >= 2) ? !!atoi(words[1].c_str()) : !useIcons;
		} break;
		case hashString("unitexp"): {
			if (words.size() >= 2)
				unitExponent = atof(words[1].c_str());

			UpdateGeometry();
		} break;
		case hashString("unitsize"): {
			if (words.size() >= 2)
				unitBaseSize = atof(words[1].c_str());

			unitBaseSize = std::max(0.0f, unitBaseSize);
			UpdateGeometry();
		} break;
		case hashString("drawcommands"): {
			if (words.size() >= 2) {
				drawCommands = std::max(0, atoi(words[1].c_str()));
			} else {
				drawCommands = (drawCommands > 0) ? 0 : 1;
			}
		} break;
		case hashString("drawprojectiles"): {
			drawProjectiles = (words.size() >= 2) ? !!atoi(words[1].c_str()) : !drawProjectiles;
		} break;
		case hashString("simplecolors"): {
			simpleColors = (words.size() >= 2) ? !!atoi(words[1].c_str()) : !simpleColors;
		} break;

		// the following commands can not be used in dualscreen mode
		case hashString("geo"):
		case hashString("geometry"): {
			if (globalRendering->dualScreenMode)
				return;
			if (words.size() < 2)
				return;

			ParseGeometry(words[1]);
			UpdateGeometry();
		} break;

		case hashString("min"):
		case hashString("minimize"): {
			if (globalRendering->dualScreenMode)
				return;

			minimized = (words.size() >= 2) ? !!atoi(words[1].c_str()) : !minimized;
		} break;

		case hashString("max"):
		case hashString("maximize"):
		case hashString("maxspect"): {
			if (globalRendering->dualScreenMode)
				return;

			const bool   isMaximized = maximized;
			const bool wantMaximized = (words.size() >= 2) ? !!atoi(words[1].c_str()) : !isMaximized;

			if (isMaximized != wantMaximized)
				ToggleMaximized(command == "maxspect");
		} break;

		default: {
		} break;
	}
}

/******************************************************************************/

void CMiniMap::SetGeometry(int px, int py, int sx, int sy)
{
	curPos = {px, py};
	curDim = {sx, sy};

	UpdateGeometry();
}


void CMiniMap::UpdateGeometry()
{
	// keep the same distance to the top
	curPos.y -= (lastWindowSizeY - globalRendering->viewSizeY);

	if (globalRendering->dualScreenMode) {
		curDim.x = globalRendering->viewSizeX;
		curDim.y = globalRendering->viewSizeY;
		curPos.x = (globalRendering->viewSizeX - globalRendering->viewPosX);
		curPos.y = 0;
	}
	else if (maximized) {
		SetMaximizedGeometry();
	}
	else {
		curDim.x = Clamp(curDim.x, 1, globalRendering->viewSizeX);
		curDim.y = Clamp(curDim.y, 1, globalRendering->viewSizeY);

		curPos.y = std::max(slaveDrawMode ? 0 : buttonSize, curPos.y);
		curPos.y = std::min(globalRendering->viewSizeY - curDim.y, curPos.y);
		curPos.x = Clamp(curPos.x, 0, globalRendering->viewSizeX - curDim.x);
	}


	{
		// heightmap to minimap
		viewMats[1].LoadIdentity();
		viewMats[1].Translate(UpVector);
		viewMats[1].Scale({1.0f / mapDims.mapx, -1.0f / mapDims.mapy, 1.0f});

		// engine draws minimap in 0..1 range, Lua uses 0..minimapSize{X,Y}
		viewMats[2].LoadIdentity();
		viewMats[2].Scale({1.0f / curDim.x, 1.0f / curDim.y, 1.0f});

		projMats[0] = CMatrix44f::ClipOrthoProj01(globalRendering->supportClipSpaceControl * 1.0f);
		projMats[1] = CMatrix44f::ClipOrthoProj(0.0f, 1.0f, 0.0f, 1.0f, 0.0f, -1.0f, globalRendering->supportClipSpaceControl * 1.0f);
		projMats[2] = projMats[1];
	}


	lastWindowSizeX = globalRendering->viewSizeX;
	lastWindowSizeY = globalRendering->viewSizeY;

	// setup the unit scaling
	const float w = float(curDim.x);
	const float h = float(curDim.y);
	const float mapx = float(mapDims.mapx * SQUARE_SIZE);
	const float mapy = float(mapDims.mapy * SQUARE_SIZE);
	const float ref  = unitBaseSize / math::pow((200.0f * 200.0f), unitExponent);
	const float dpr  = ref * math::pow((w * h), unitExponent);

	unitSizeX = dpr * (mapx / w);
	unitSizeY = dpr * (mapy / h);
	unitSelectRadius = fastmath::apxsqrt(unitSizeX * unitSizeY);

	// in mouse coordinates
	mapBox.xmin = curPos.x;
	mapBox.xmax = mapBox.xmin + curDim.x - 1;
	mapBox.ymin = globalRendering->viewSizeY - (curPos.y + curDim.y);
	mapBox.ymax = mapBox.ymin + curDim.y - 1;

	// FIXME:
	//   also need to make sure we can leave maximized-mode when !maxspect (in
	//   which case the buttons should be drawn on top of map, not outside it)
	if (!maximized || maxspect) {
		// work right (resizeBox) to left (minimizeBox)
		resizeBox.xmax   = mapBox.xmax;
		resizeBox.xmin   = resizeBox.xmax - (buttonSize - 1);

		moveBox.xmax     = resizeBox.xmax   - buttonSize;
		moveBox.xmin     = resizeBox.xmin   - buttonSize;

		maximizeBox.xmax = moveBox.xmax     - buttonSize;
		maximizeBox.xmin = moveBox.xmin     - buttonSize;

		minimizeBox.xmax = maximizeBox.xmax - buttonSize;
		minimizeBox.xmin = maximizeBox.xmin - buttonSize;

		const int ymin = (mapBox.ymax + 1) + 3; // 3 for the white|black|white
		const int ymax = ymin + (buttonSize - 1);
		resizeBox.ymin = moveBox.ymin = maximizeBox.ymin = minimizeBox.ymin = ymin;
		resizeBox.ymax = moveBox.ymax = maximizeBox.ymax = minimizeBox.ymax = ymax;

		buttonBox.xmin = minimizeBox.xmin;
		buttonBox.xmax = mapBox.xmax;
		buttonBox.ymin = ymin - 3;
		buttonBox.ymax = ymax;
	} else {
		// work left to right
		minimizeBox.xmin = mapBox.xmin;
		minimizeBox.xmax = minimizeBox.xmin + (buttonSize - 1);

		maximizeBox.xmin = minimizeBox.xmin + buttonSize;
		maximizeBox.xmax = minimizeBox.xmax + buttonSize;

		// dead buttons
		resizeBox.xmin = resizeBox.ymin = moveBox.xmin = moveBox.ymin =  0;
		resizeBox.xmax = resizeBox.ymax = moveBox.xmax = moveBox.ymax = -1;

		const int ymin = mapBox.ymin;
		const int ymax = ymin + (buttonSize - 1);
		maximizeBox.ymin = minimizeBox.ymin = ymin;
		maximizeBox.ymax = minimizeBox.ymax = ymax;

		buttonBox.xmin = minimizeBox.xmin;
		buttonBox.xmax = maximizeBox.xmax;
		buttonBox.ymin = ymin - 3;
		buttonBox.ymax = ymax;
	}
}


/******************************************************************************/

void CMiniMap::MoveView(int x, int y)
{
	const float3 clickPos = GetMapPosition(x, y);

	camHandler->CameraTransition(0.0f);
	camHandler->GetCurrentController().SetPos({clickPos.x, 0.0f, clickPos.z});
	unitTracker.Disable();
}


void CMiniMap::SelectUnits(int x, int y)
{
	const CUnit *_lastClicked = lastClicked;
	lastClicked = nullptr;

	if (!KeyInput::GetKeyModState(KMOD_SHIFT) && !KeyInput::GetKeyModState(KMOD_CTRL))
		selectedUnitsHandler.ClearSelected();

	CMouseHandler::ButtonPressEvt& bp = mouse->buttons[SDL_BUTTON_LEFT];

	if (fullProxy && (bp.movement > 4)) {
		// use a selection box
		const float3 newMapPos = GetMapPosition(x, y);
		const float3 oldMapPos = GetMapPosition(bp.x, bp.y);

		const float xmin = std::min(oldMapPos.x, newMapPos.x);
		const float xmax = std::max(oldMapPos.x, newMapPos.x);
		const float zmin = std::min(oldMapPos.z, newMapPos.z);
		const float zmax = std::max(oldMapPos.z, newMapPos.z);

		const float4  planeRight(-RgtVector,  xmin);
		const float4   planeLeft( RgtVector, -xmax);
		const float4    planeTop( FwdVector, -zmax);
		const float4 planeBottom(-FwdVector,  zmin);

		selectedUnitsHandler.HandleUnitBoxSelection(planeRight, planeLeft, planeTop, planeBottom);
	} else {
		// Single unit
		const float3 pos = GetMapPosition(x, y);

		CUnit* unit;
		if (gu->spectatingFullSelect) {
			unit = CGameHelper::GetClosestUnit(pos, unitSelectRadius);
		} else {
			unit = CGameHelper::GetClosestFriendlyUnit(nullptr, pos, unitSelectRadius, gu->myAllyTeam);
		}

		selectedUnitsHandler.HandleSingleUnitClickSelection(lastClicked = unit, false, bp.lastRelease >= (gu->gameTime - mouse->doubleClickTime) && unit == _lastClicked);
	}

	bp.lastRelease = gu->gameTime;
}

/******************************************************************************/

bool CMiniMap::MousePress(int x, int y, int button)
{
	if (minimized) {
		if ((x < buttonSize) && (y < buttonSize)) {
			minimized = false;
			return true;
		}

		return false;
	}

	const bool inMap = mapBox.Inside(x, y);
	const bool inButtons = buttonBox.Inside(x, y);

	if (!inMap && !inButtons)
		return false;

	if (button == SDL_BUTTON_LEFT) {
		if (inMap && (guihandler->inCommand >= 0)) {
			proxyMode = true;
			ProxyMousePress(x, y, button);
			return true;
		}
		if (showButtons && inButtons) {
			if (moveBox.Inside(x, y)) {
				mouseMove = true;
				return true;
			}
			else if (resizeBox.Inside(x, y)) {
				mouseResize = true;
				return true;
			}
			else if (minimizeBox.Inside(x, y) || maximizeBox.Inside(x, y)) {
				return true;
			}
		}
		if (inMap && !mouse->buttons[SDL_BUTTON_LEFT].chorded) {
			selecting = true;
			return true;
		}
	}
	else if (inMap) {
		if ((fullProxy && (button == SDL_BUTTON_MIDDLE)) || (!fullProxy && (button == SDL_BUTTON_RIGHT))) {
			MoveView(x, y);

			if (maximized) {
				ToggleMaximized(false);
			} else {
				mouseLook = true;
			}

			return true;
		}
		else if (fullProxy && (button == SDL_BUTTON_RIGHT)) {
			proxyMode = true;
			ProxyMousePress(x, y, button);
			return true;
		}
	}

	return false;
}


void CMiniMap::MouseMove(int x, int y, int dx, int dy, int button)
{
	if (mouseMove) {
		curPos.x += dx;
		curPos.y -= dy;
		curPos.x = std::max(0, curPos.x);

		if (globalRendering->dualScreenMode) {
			curPos.x = std::min((2 * globalRendering->viewSizeX) - curDim.x, curPos.x);
		} else {
			curPos.x = std::min(globalRendering->viewSizeX - curDim.x, curPos.x);
		}
		curPos.y = std::max(5, std::min(globalRendering->viewSizeY - curDim.y, curPos.y));

		UpdateGeometry();
		return;
	}

	if (mouseResize) {
		curPos.y -= dy;
		curDim.x += dx;
		curDim.y += dy;
		curDim.y  = std::min(globalRendering->viewSizeY, curDim.y);

		if (globalRendering->dualScreenMode) {
			curDim.x = std::min(2 * globalRendering->viewSizeX, curDim.x);
		} else {
			curDim.x = std::min(globalRendering->viewSizeX, curDim.x);
		}

		if (KeyInput::GetKeyModState(KMOD_SHIFT))
			curDim.x = (curDim.y * mapDims.mapx) / mapDims.mapy;

		curDim.x = std::max(5, curDim.x);
		curDim.y = std::max(5, curDim.y);
		curPos.y = std::min(globalRendering->viewSizeY - curDim.y, curPos.y);

		UpdateGeometry();
		return;
	}

	if (mouseLook && mapBox.Inside(x, y)) {
		MoveView(x,y);
		return;
	}
}


void CMiniMap::MouseRelease(int x, int y, int button)
{
	if (mouseMove || mouseResize || mouseLook) {
		mouseMove = false;
		mouseResize = false;
		mouseLook = false;
		proxyMode = false;
		return;
	}

	if (proxyMode) {
		ProxyMouseRelease(x, y, button);
		proxyMode = false;
		return;
	}

	if (selecting) {
		SelectUnits(x, y);
		selecting = false;
		return;
	}

	if (button == SDL_BUTTON_LEFT) {
		if (showButtons && maximizeBox.Inside(x, y)) {
			ToggleMaximized(!!KeyInput::GetKeyModState(KMOD_SHIFT));
			return;
		}

		if (showButtons && minimizeBox.Inside(x, y)) {
			minimized = true;
			return;
		}
	}
}

/******************************************************************************/

CUnit* CMiniMap::GetSelectUnit(const float3& pos) const
{
	CUnit* unit = CGameHelper::GetClosestUnit(pos, unitSelectRadius);

	if (unit == nullptr)
		return unit;

	if ((unit->losStatus[gu->myAllyTeam] & (LOS_INLOS | LOS_INRADAR)) || gu->spectatingFullView)
		return unit;

	return nullptr;
}


float3 CMiniMap::GetMapPosition(int x, int y) const
{
	const float mapX = float3::maxxpos + 1.0f;
	const float mapZ = float3::maxzpos + 1.0f;

	// mouse coordinate-origin is top-left of screen while
	// the minimap origin is in its bottom-left corner, so
	//   (x =     0, y =     0) maps to world-coors (   0, h,    0)
	//   (x = dim.x, y =     0) maps to world-coors (mapX, h,    0)
	//   (x = dim.x, y = dim.y) maps to world-coors (mapX, h, mapZ)
	//   (x =     0, y = dim.y) maps to world-coors (   0, h, mapZ)
	const float sx = Clamp(float(x -                               tmpPos.x            ) / curDim.x, 0.0f, 1.0f);
	const float sz = Clamp(float(y - (globalRendering->viewSizeY - tmpPos.y - curDim.y)) / curDim.y, 0.0f, 1.0f);

	return {mapX * sx, readMap->GetCurrMaxHeight(), mapZ * sz};
}


void CMiniMap::ProxyMousePress(int x, int y, int button)
{
	float3 mapPos = GetMapPosition(x, y);
	const CUnit* unit = GetSelectUnit(mapPos);

	if (unit != nullptr) {
		if (gu->spectatingFullView) {
			mapPos = unit->midPos;
		} else {
			mapPos = unit->GetObjDrawErrorPos(gu->myAllyTeam);
			mapPos.y = readMap->GetCurrMaxHeight();
		}
	}

	CMouseHandler::ButtonPressEvt& bp = mouse->buttons[button];
	bp.camPos = mapPos;
	bp.dir = -UpVector;

	guihandler->MousePress(x, y, -button);
}


void CMiniMap::ProxyMouseRelease(int x, int y, int button)
{
	float3 mapPos = GetMapPosition(x, y);
	const CUnit* unit = GetSelectUnit(mapPos);

	if (unit != nullptr) {
		if (gu->spectatingFullView) {
			mapPos = unit->midPos;
		} else {
			mapPos = unit->GetObjDrawErrorPos(gu->myAllyTeam);
			mapPos.y = readMap->GetCurrMaxHeight();
		}
	}

	guihandler->MouseRelease(x, y, -button, mapPos, -UpVector);
}


/******************************************************************************/

bool CMiniMap::IsAbove(int x, int y)
{
	if (minimized)
		return ((x < buttonSize) && (y < buttonSize));

	if (mapBox.Inside(x, y))
		return true;

	if (showButtons && buttonBox.Inside(x, y))
		return true;

	return false;
}


std::string CMiniMap::GetTooltip(int x, int y)
{
	if (minimized)
		return "Unminimize map";

	if (buttonBox.Inside(x, y)) {
		if (resizeBox.Inside(x, y))
			return "Resize map\n(SHIFT to maintain aspect ratio)";

		if (moveBox.Inside(x, y))
			return "Move map";

		if (maximizeBox.Inside(x, y)) {
			if (!maximized)
				return "Maximize map\n(SHIFT to maintain aspect ratio)";

			return "Unmaximize map";
		}

		if (minimizeBox.Inside(x, y))
			return "Minimize map";
	}

	const std::string buildTip = std::move(guihandler->GetBuildTooltip());
	if (!buildTip.empty())
		return buildTip;

	const float3 wpos = GetMapPosition(x, y);
	const CUnit* unit = GetSelectUnit(wpos);
	if (unit != nullptr)
		return (std::move(CTooltipConsole::MakeUnitString(unit)));

	const std::string selTip = std::move(selectedUnitsHandler.GetTooltip());
	if (!selTip.empty())
		return selTip;

	return (std::move(CTooltipConsole::MakeGroundString({wpos.x, CGround::GetHeightReal(wpos.x, wpos.z, false), wpos.z})));
}


void CMiniMap::AddNotification(float3 pos, float3 color, float alpha)
{
	Notification n;
	n.pos = pos;
	n.color = {color, alpha};
	n.creationTime = gu->gameTime;

	notes.push_back(n);
}


/******************************************************************************/

void CMiniMap::DrawCircle(CVertexArray* va, const float4& pos, const float4& color) const
{
	#if 0
	CMatrix44f vm = viewMats[0];

	vm.Translate(pos);
	vm.Scale({pos.w, 1.0f, pos.w});
	#else
	GL::PushMatrix();
	GL::Translate(pos.x, pos.y, pos.z);
	GL::Scale(pos.w, 1.0f, pos.w);
	#endif

	const float xPixels = pos.w * float(curDim.x) / float(mapDims.mapx * SQUARE_SIZE);
	const float yPixels = pos.w * float(curDim.y) / float(mapDims.mapy * SQUARE_SIZE);

	const int lod = (int)(0.25 * math::log2(1.0f + (xPixels * yPixels)));
	const int divs = 1 << (Clamp(lod, 0, 6 - 1) + 3);

	for (int d = 0; d < divs; d++) {
		const float step = d * 1.0f / divs;
		const float rads = math::TWOPI * step;

		#if 0
		va->AddVertexC(vm * float3{std::sin(rads), 0.0f, std::cos(rads)}, SColor(&color.x));
		#else
		va->AddVertexC({std::sin(rads), 0.0f, std::cos(rads)}, SColor(&color.x));
		#endif
	}

	va->DrawArrayC(GL_LINE_LOOP);

	#if 0
	#else
	GL::PopMatrix();
	#endif
}

void CMiniMap::DrawSurfaceCircle(CVertexArray* va, const float4& pos, const float4& color, unsigned int)
{
	minimap->DrawCircle(va, pos, color);
}


void CMiniMap::EnterNormalizedCoors(bool pushMatrix, bool dualScreen) const
{
	if (pushMatrix)
		GL::PushMatrix();

	// switch to normalized minimap coords
	if (dualScreen) {
		glAttribStatePtr->ViewPort(curPos.x, curPos.y, curDim.x, curDim.y);
	} else {
		GL::Translate(curPos.x * globalRendering->pixelX, curPos.y * globalRendering->pixelY, 0.0f);
	}

	GL::Scale(curDim.x * globalRendering->pixelX, curDim.y * globalRendering->pixelY, 1.0f);
}

void CMiniMap::LeaveNormalizedCoors(bool popMatrix, bool dualScreen) const
{
	if (popMatrix)
		GL::PopMatrix();

	if (dualScreen)
		glAttribStatePtr->ViewPort(globalRendering->viewPosX, 0, globalRendering->viewSizeX, globalRendering->viewSizeY);
}

/******************************************************************************/

void CMiniMap::Update()
{
	// need this because UpdateTextureCache sets curPos={0,0}
	// (while calling DrawForReal, which can reach GetMapPos)
	tmpPos = curPos;

	if (minimized || curDim.x == 0 || curDim.y == 0)
		return;

	static spring_time nextDrawScreen = spring_gettime();

	if (spring_gettime() <= nextDrawScreen)
		return;

	float refreshRate = minimapRefreshRate;

	if (minimapRefreshRate == 0) {
		const float viewArea = globalRendering->viewSizeX * globalRendering->viewSizeY;
		const float mmapArea = (curDim.x * curDim.y) / viewArea;
		refreshRate = (mmapArea >= 0.45f) ? 60 : (mmapArea > 0.15f) ? 25 : 15;
	}
	nextDrawScreen = spring_gettime() + spring_msecs(1000.0f / refreshRate);

	fbo.Bind();
	ResizeTextureCache();
	fbo.Bind();
	UpdateTextureCache();

	// gets done in CGame
	// fbo.Unbind();
}


void CMiniMap::ResizeTextureCache()
{
	#ifdef HEADLESS
	return;
	#endif

	if (minimapTexSize == curDim)
		return;

	minimapTexSize = curDim;

	if ((multisampledFBO = (fbo.GetMaxSamples() > 1))) {
		// multisampled FBO we are render to
		fbo.Detach(GL_COLOR_ATTACHMENT0); // delete old RBO
		fbo.CreateRenderBufferMultisample(GL_COLOR_ATTACHMENT0, GL_RGBA8, minimapTexSize.x, minimapTexSize.y, 4);
		//fbo.CreateRenderBuffer(GL_DEPTH_ATTACHMENT, GL_DEPTH_COMPONENT16, minimapTexSize.x, minimapTexSize.y);

		if (!(multisampledFBO = fbo.CheckStatus("MINIMAP")))
			fbo.Detach(GL_COLOR_ATTACHMENT0);
	}

	glDeleteTextures(1, &minimapTextureID);
	glGenTextures(1, &minimapTextureID);

	glBindTexture(GL_TEXTURE_2D, minimapTextureID);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, minimapTexSize.x, minimapTexSize.y, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);

	if (multisampledFBO) {
		// resolve FBO with attached final texture target
		fboResolve.Bind();
		fboResolve.AttachTexture(minimapTextureID);

		if (fboResolve.CheckStatus("MINIMAP-RESOLVE"))
			return;

		throw opengl_error("[MiniMap::ResizeTextureCache] bad multisample framebuffer");
	}

	// directly render to texture without multisampling (fallback solution)
	fbo.Bind();
	fbo.AttachTexture(minimapTextureID);

	if (fbo.CheckStatus("MINIMAP-RESOLVE"))
		return;

	throw opengl_error("[MiniMap::ResizeTextureCache] bad fallback framebuffer");
}


void CMiniMap::UpdateTextureCache()
{
	#if 0
	viewMats[0].LoadIdentity();
	#else
	glSpringMatrix2dSetupPV(0.0f, 1.0f, 0.0f, 1.0f, -1.0f, 1.0f,  true, true);
	#endif

	{
		curPos = {0, 0};

			// draw minimap into FBO
			glAttribStatePtr->ViewPort(0, 0, minimapTexSize.x, minimapTexSize.y);
			glAttribStatePtr->ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
			glAttribStatePtr->Clear(GL_COLOR_BUFFER_BIT);

			DrawForReal(false, true, false);

		curPos = tmpPos;
	}

	glSpringMatrix2dResetPV(true, true);

	// resolve multisampled FBO if there is one
	if (!multisampledFBO)
		return;

	glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo.fboId);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fboResolve.fboId);
	glBlitFramebuffer(
		0, 0, minimapTexSize.x, minimapTexSize.y,
		0, 0, minimapTexSize.x, minimapTexSize.y,
		GL_COLOR_BUFFER_BIT, GL_NEAREST);
}


/******************************************************************************/

void CMiniMap::Draw()
{
	if (slaveDrawMode)
		return;

	SCOPED_TIMER("Draw::Screen::InputReceivers::MiniMap");
	// Draw Border
	{
		glAttribStatePtr->EnableBlendMask();
		glAttribStatePtr->BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glAttribStatePtr->PushDepthBufferBit();
		glAttribStatePtr->DisableDepthTest();
		glAttribStatePtr->DepthFunc(GL_LEQUAL);
		glAttribStatePtr->DisableDepthMask();
		glDisable(GL_TEXTURE_2D);
		GL::MatrixMode(GL_MODELVIEW);

		GL::RenderDataBufferC * rdBufferC  = GL::GetRenderBufferC ();
		GL::RenderDataBufferTC* rdBufferTC = GL::GetRenderBufferTC();

		if (minimized) {
			DrawMinimizedButton(rdBufferC, rdBufferTC);
			glAttribStatePtr->PopBits();
			glEnable(GL_TEXTURE_2D);
			return;
		}

		// draw the frameborder
		if (!globalRendering->dualScreenMode) {
			DrawFrame(rdBufferC);
			DrawButtons(rdBufferC, rdBufferTC);
		}

		glAttribStatePtr->PopBits();
	}

	glSpringMatrix2dSetupPV(0.0f, 1.0f, 0.0f, 1.0f, -1.0f, 1.0f);
	// draw minimap itself
	DrawForReal(true, false, false);
	glSpringMatrix2dResetPV();
}



// Update->UpdateTextureCache->DrawForReal(false)->RenderCachedTexture
// Draw->DrawForReal(true)->RenderCachedTexture
// LuaOpenGL::DrawMiniMap->DrawForReal(true OR false)->RenderCachedTexture
//
// DrawForReal->DrawBackground->{readMap->DrawMiniMap (with multi-texturing, can not use standard renderbuffer shader!}
void CMiniMap::DrawForReal(bool useNormalizedCoors, bool updateTex, bool luaCall)
{
	if (minimized)
		return;

	glActiveTexture(GL_TEXTURE0);

	if (!updateTex) {
		RenderCachedTexture(useNormalizedCoors);
		return;
	}

	glAttribStatePtr->PushDepthBufferBit();
	glAttribStatePtr->EnableBlendMask();
	glAttribStatePtr->BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glAttribStatePtr->DisableDepthTest();
	glAttribStatePtr->DepthFunc(GL_LEQUAL);
	glAttribStatePtr->DisableDepthMask();
	glDisable(GL_TEXTURE_2D);
	GL::MatrixMode(GL_MODELVIEW);

	if (useNormalizedCoors)
		EnterNormalizedCoors(true, globalRendering->dualScreenMode);

	setSurfaceCircleFuncVA(DrawSurfaceCircle);
	cursorIcons.Enable(false);

	// clip everything outside of the minimap box
	SetClipPlanes(false);
	glEnable(GL_CLIP_PLANE0);
	glEnable(GL_CLIP_PLANE1);
	glEnable(GL_CLIP_PLANE2);
	glEnable(GL_CLIP_PLANE3);

	DrawBackground();

	// allow Lua scripts to overdraw the background image
	SetClipPlanes(true);
	eventHandler.DrawInMiniMapBackground();
	SetClipPlanes(false);

	DrawUnitIcons();
	DrawWorldStuff();

	if (useNormalizedCoors)
		LeaveNormalizedCoors(true, false);

	glAttribStatePtr->PopBits();
	glEnable(GL_TEXTURE_2D);

	// allow Lua scripts to draw into the minimap
	SetClipPlanes(true);
	eventHandler.DrawInMiniMap();

	if (!updateTex) {
		EnterNormalizedCoors(true, false);
		DrawCameraFrustumAndMouseSelection();
		LeaveNormalizedCoors(true, false);
	}

	// reset GL state after Lua
	LeaveNormalizedCoors(false, useNormalizedCoors && globalRendering->dualScreenMode);

	// disable ClipPlanes
	glDisable(GL_CLIP_PLANE0);
	glDisable(GL_CLIP_PLANE1);
	glDisable(GL_CLIP_PLANE2);
	glDisable(GL_CLIP_PLANE3);

	cursorIcons.Enable(true);
	setSurfaceCircleFuncVA(nullptr);
}


/******************************************************************************/
/******************************************************************************/

void CMiniMap::DrawCameraFrustumAndMouseSelection()
{
	glDisable(GL_TEXTURE_2D);

	// clip everything outside of the minimap box
	SetClipPlanes(false);
	glEnable(GL_CLIP_PLANE0);
	glEnable(GL_CLIP_PLANE1);
	glEnable(GL_CLIP_PLANE2);
	glEnable(GL_CLIP_PLANE3);

	// switch to top-down map/world coords (NB: z and y are swapped wrt real map/world coords)
	GL::PushMatrix();
	GL::Translate(0.0f, +1.0f, 0.0f);
	GL::Scale(+1.0f / (mapDims.mapx * SQUARE_SIZE), -1.0f / (mapDims.mapy * SQUARE_SIZE), 1.0f);

	if (!minimap->maximized) {
		// draw the camera frustum lines
		// CCamera* cam = CCameraHandler::GetCamera(CCamera::CAMTYPE_SHADOW);
		CCamera* cam = CCameraHandler::GetCamera(CCamera::CAMTYPE_PLAYER);

		cam->CalcFrustumLines(readMap->GetCurrAvgHeight(), readMap->GetCurrAvgHeight(), 1.0f, true);
		cam->ClipFrustumLines(-100.0f, mapDims.mapy * SQUARE_SIZE + 100.0f, true);

		const CCamera::FrustumLine* negLines = cam->GetNegFrustumLines();

		CVertexArray* va = GetVertexArray();
		va->Initialize();
		va->EnlargeArrays(4 * 2, 0, VA_SIZE_2D0);

		for (int idx = 0; idx < /*negLines[*/4/*].sign*/; idx++) {
			const CCamera::FrustumLine& fl = negLines[idx];

			if (fl.minz >= fl.maxz)
				continue;

			va->AddVertexQ2d0((fl.dir * fl.minz) + fl.base, fl.minz);
			va->AddVertexQ2d0((fl.dir * fl.maxz) + fl.base, fl.maxz);
		}

		glAttribStatePtr->LineWidth(2.5f);
		glColor4f(0, 0, 0, 0.5f);
		va->DrawArray2d0(GL_LINES);

		glAttribStatePtr->LineWidth(1.5f);
		glColor4f(1, 1, 1, 0.75f);
		va->DrawArray2d0(GL_LINES);
		glAttribStatePtr->LineWidth(1.0f);
	}


	// selection box
	glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
	CMouseHandler::ButtonPressEvt& bp = mouse->buttons[SDL_BUTTON_LEFT];
	if (selecting && fullProxy && (bp.movement > 4)) {
		const float3 oldMapPos = GetMapPosition(bp.x, bp.y);
		const float3 newMapPos = GetMapPosition(mouse->lastx, mouse->lasty);
		glColor4fv(cmdColors.mouseBox);
		//glAttribStatePtr->BlendFunc((GLenum)cmdColors.MouseBoxBlendSrc(), (GLenum)cmdColors.MouseBoxBlendDst());
		glAttribStatePtr->LineWidth(cmdColors.MouseBoxLineWidth());

		CVertexArray* va = GetVertexArray();
		va->Initialize();
		va->EnlargeArrays(4, 0, VA_SIZE_2D0);
			va->AddVertexQ2d0(oldMapPos.x, oldMapPos.z);
			va->AddVertexQ2d0(newMapPos.x, oldMapPos.z);
			va->AddVertexQ2d0(newMapPos.x, newMapPos.z);
			va->AddVertexQ2d0(oldMapPos.x, newMapPos.z);
		va->DrawArray2d0(GL_LINE_LOOP);

		glAttribStatePtr->LineWidth(1.0f);
		//glAttribStatePtr->BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	}

	DrawNotes();

	// disable ClipPlanes
	glDisable(GL_CLIP_PLANE0);
	glDisable(GL_CLIP_PLANE1);
	glDisable(GL_CLIP_PLANE2);
	glDisable(GL_CLIP_PLANE3);

	GL::PopMatrix();
	glEnable(GL_TEXTURE_2D);
}



void CMiniMap::DrawMinimizedButton(GL::RenderDataBufferC* rdBufferC, GL::RenderDataBufferTC* rdBufferTC)
{
	const int bs = buttonSize + 0;

	const float px = globalRendering->pixelX;
	const float py = globalRendering->pixelY;

	const float xmin = (globalRendering->viewPosX + 1     ) * px;
	const float xmax = (globalRendering->viewPosX + 1 + bs) * px;
	const float ymin = 1.0f - (1 + bs) * py;
	const float ymax = 1.0f - (1     ) * py;

	{
		glBindTexture(GL_TEXTURE_2D, buttonsTextureID);

		rdBufferTC->GetShader()->Enable();
		rdBufferTC->GetShader()->SetUniformMatrix4x4<const char*, float>("u_movi_mat", false, CMatrix44f::Identity());
		rdBufferTC->GetShader()->SetUniformMatrix4x4<const char*, float>("u_proj_mat", false, projMats[0]);
		rdBufferTC->SafeAppend({{xmin, ymin, 0.0f}, minimizeBox.xminTx, minimizeBox.yminTx, {1.0f, 1.0f, 1.0f, 1.0f}});
		rdBufferTC->SafeAppend({{xmax, ymin, 0.0f}, minimizeBox.xmaxTx, minimizeBox.yminTx, {1.0f, 1.0f, 1.0f, 1.0f}});
		rdBufferTC->SafeAppend({{xmax, ymax, 0.0f}, minimizeBox.xmaxTx, minimizeBox.ymaxTx, {1.0f, 1.0f, 1.0f, 1.0f}});
		rdBufferTC->SafeAppend({{xmin, ymax, 0.0f}, minimizeBox.xminTx, minimizeBox.ymaxTx, {1.0f, 1.0f, 1.0f, 1.0f}});
		rdBufferTC->Submit(GL_QUADS);
		rdBufferTC->GetShader()->Disable();
	}
	{
		rdBufferC->GetShader()->Enable();

		// highlight
		if ((mouse->lastx + 1 <= buttonSize) && (mouse->lasty + 1 <= buttonSize)) {
			// glAttribStatePtr->BlendFunc(GL_SRC_ALPHA, GL_ONE);
			rdBufferC->SafeAppend({{xmin, ymin, 0.0f}, {1.0f, 1.0f, 1.0f, 0.4f}});
			rdBufferC->SafeAppend({{xmax, ymin, 0.0f}, {1.0f, 1.0f, 1.0f, 0.4f}});
			rdBufferC->SafeAppend({{xmax, ymax, 0.0f}, {1.0f, 1.0f, 1.0f, 0.4f}});
			rdBufferC->SafeAppend({{xmin, ymax, 0.0f}, {1.0f, 1.0f, 1.0f, 0.4f}});
			rdBufferC->Submit(GL_QUADS);
			// glAttribStatePtr->BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		}

		// outline
		rdBufferC->SafeAppend({{xmin - 0.5f * px, ymax + 0.5f * py, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}});
		rdBufferC->SafeAppend({{xmin - 0.5f * px, ymin - 0.5f * py, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}});
		rdBufferC->SafeAppend({{xmax + 0.5f * px, ymin - 0.5f * py, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}});
		rdBufferC->SafeAppend({{xmax + 0.5f * px, ymax + 0.5f * py, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}});
		rdBufferC->Submit(GL_LINE_LOOP);
		rdBufferC->GetShader()->Disable();
	}
}



void CMiniMap::IntBox::DrawBox(GL::RenderDataBufferC* rdBufferC) const
{
	const float vsx = globalRendering->viewSizeX;
	const float vsy = globalRendering->viewSizeY;

	const float tlx =         (xmin      / vsx);
	const float tly = 1.0f -  (ymin      / vsy);
	const float brx =        ((xmax + 1) / vsx);
	const float bry = 1.0f - ((ymax + 1) / vsy);

	rdBufferC->SafeAppend({{tlx, tly, 0.0f}, {1.0f, 1.0f, 1.0f, 0.4f}});
	rdBufferC->SafeAppend({{brx, tly, 0.0f}, {1.0f, 1.0f, 1.0f, 0.4f}});
	rdBufferC->SafeAppend({{brx, bry, 0.0f}, {1.0f, 1.0f, 1.0f, 0.4f}});
	rdBufferC->SafeAppend({{tlx, bry, 0.0f}, {1.0f, 1.0f, 1.0f, 0.4f}});
}

void CMiniMap::IntBox::DrawTextureBox(GL::RenderDataBufferTC* rdBufferTC) const
{
	const float px = globalRendering->pixelX;
	const float py = globalRendering->pixelY;

	rdBufferTC->SafeAppend({{(xmin    ) * px, 1.0f - (ymin    ) * py, 0.0f}, xminTx, yminTx, color});
	rdBufferTC->SafeAppend({{(xmax + 1) * px, 1.0f - (ymin    ) * py, 0.0f}, xmaxTx, yminTx, color});
	rdBufferTC->SafeAppend({{(xmax + 1) * px, 1.0f - (ymax + 1) * py, 0.0f}, xmaxTx, ymaxTx, color});
	rdBufferTC->SafeAppend({{(xmin    ) * px, 1.0f - (ymax + 1) * py, 0.0f}, xminTx, ymaxTx, color});
}


void CMiniMap::DrawFrame(GL::RenderDataBufferC* rdBufferC)
{
	// outline
	glAttribStatePtr->LineWidth(1.0f);

	const float px = globalRendering->pixelX;
	const float py = globalRendering->pixelY;

	rdBufferC->GetShader()->Enable();
	rdBufferC->GetShader()->SetUniformMatrix4x4<const char*, float>("u_movi_mat", false, CMatrix44f::Identity());
	rdBufferC->GetShader()->SetUniformMatrix4x4<const char*, float>("u_proj_mat", false, projMats[0]);

	rdBufferC->SafeAppend({{(curPos.x            - 2 + 0.5f) * px, (curPos.y            - 2 + 0.5f) * py, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}});
	rdBufferC->SafeAppend({{(curPos.x            - 2 + 0.5f) * px, (curPos.y + curDim.y + 2 - 0.5f) * py, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}});
	rdBufferC->SafeAppend({{(curPos.x + curDim.x + 2 - 0.5f) * px, (curPos.y + curDim.y + 2 - 0.5f) * py, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}});
	rdBufferC->SafeAppend({{(curPos.x + curDim.x + 2 - 0.5f) * px, (curPos.y            - 2 + 0.5f) * py, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}});
	rdBufferC->Submit(GL_LINE_LOOP);

	rdBufferC->SafeAppend({{(curPos.x            - 1 + 0.5f) * px, (curPos.y            - 1 + 0.5f) * py, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}});
	rdBufferC->SafeAppend({{(curPos.x            - 1 + 0.5f) * px, (curPos.y + curDim.y + 1 - 0.5f) * py, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}});
	rdBufferC->SafeAppend({{(curPos.x + curDim.x + 1 - 0.5f) * px, (curPos.y + curDim.y + 1 - 0.5f) * py, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}});
	rdBufferC->SafeAppend({{(curPos.x + curDim.x + 1 - 0.5f) * px, (curPos.y            - 1 + 0.5f) * py, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}});
	rdBufferC->Submit(GL_LINE_LOOP);

	rdBufferC->GetShader()->Disable();
}

void CMiniMap::DrawButtons(GL::RenderDataBufferC* rdBufferC, GL::RenderDataBufferTC* rdBufferTC)
{
	const int x = mouse->lastx;
	const int y = mouse->lasty;

	// update the showButtons state
	if (!showButtons) {
		if (!(showButtons = (mapBox.Inside(x, y) && (buttonSize > 0) && !globalRendering->dualScreenMode)))
			return;

	} else if (!mouseMove && !mouseResize && !mapBox.Inside(x, y) && !buttonBox.Inside(x, y)) {
		showButtons = false;
		return;
	}

	{
		glBindTexture(GL_TEXTURE_2D, buttonsTextureID);

		rdBufferTC->GetShader()->Enable();
		rdBufferTC->GetShader()->SetUniformMatrix4x4<const char*, float>("u_movi_mat", false, CMatrix44f::Identity());
		rdBufferTC->GetShader()->SetUniformMatrix4x4<const char*, float>("u_proj_mat", false, projMats[0]);

		resizeBox.DrawTextureBox(rdBufferTC);
		moveBox.DrawTextureBox(rdBufferTC);
		maximizeBox.DrawTextureBox(rdBufferTC);
		minimizeBox.DrawTextureBox(rdBufferTC);

		rdBufferTC->Submit(GL_QUADS);
		rdBufferTC->GetShader()->Disable();

		glBindTexture(GL_TEXTURE_2D, 0);
	}


	// matrices are already set in DrawFrame
	rdBufferC->GetShader()->Enable();

	{
		// highlight
		// glBlendFunc(GL_SRC_ALPHA, GL_ONE);
		if (mouseResize || (!mouseMove && resizeBox.Inside(x, y))) {
			resizeBox.DrawBox(rdBufferC);
		} else if (mouseMove || (!mouseResize && moveBox.Inside(x, y))) {
			moveBox.DrawBox(rdBufferC);
		} else if (!mouseMove && !mouseResize) {
			if (minimizeBox.Inside(x, y)) {
				minimizeBox.DrawBox(rdBufferC);
			} else if (maximizeBox.Inside(x, y)) {
				maximizeBox.DrawBox(rdBufferC);
			}
		}

		rdBufferC->Submit(GL_QUADS);
		// glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	}

	{
		const float px = globalRendering->pixelX;
		const float py = globalRendering->pixelY;

		// outline the button box
		rdBufferC->SafeAppend({{(buttonBox.xmin - 1 - 0.5f) * px, 1.0f - (buttonBox.ymin + 2 - 0.5f) * py, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}});
		rdBufferC->SafeAppend({{(buttonBox.xmin - 1 - 0.5f) * px, 1.0f - (buttonBox.ymax + 2 + 0.5f) * py, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}});
		rdBufferC->SafeAppend({{(buttonBox.xmax + 2 + 0.5f) * px, 1.0f - (buttonBox.ymax + 2 + 0.5f) * py, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}});
		rdBufferC->SafeAppend({{(buttonBox.xmax + 2 + 0.5f) * px, 1.0f - (buttonBox.ymin + 2 - 0.5f) * py, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}});
		rdBufferC->Submit(GL_LINE_LOOP);

		rdBufferC->SafeAppend({{(buttonBox.xmin - 0 - 0.5f) * px, 1.0f - (buttonBox.ymin + 3 - 0.5f) * py, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}});
		rdBufferC->SafeAppend({{(buttonBox.xmin - 0 - 0.5f) * px, 1.0f - (buttonBox.ymax + 1 + 0.5f) * py, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}});
		rdBufferC->SafeAppend({{(buttonBox.xmax + 1 + 0.5f) * px, 1.0f - (buttonBox.ymax + 1 + 0.5f) * py, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}});
		rdBufferC->SafeAppend({{(buttonBox.xmax + 1 + 0.5f) * px, 1.0f - (buttonBox.ymin + 3 - 0.5f) * py, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}});
		rdBufferC->Submit(GL_LINE_LOOP);
	}

	rdBufferC->GetShader()->Disable();
}


void CMiniMap::DrawNotes()
{
	if (notes.empty())
		return;

	const float baseSize = mapDims.mapx * SQUARE_SIZE;
	CVertexArray* va = GetVertexArray();
	va->Initialize();
	std::deque<Notification>::iterator ni = notes.begin();
	while (ni != notes.end()) {
		const float age = gu->gameTime - ni->creationTime;
		if (age > 2) {
			ni = notes.erase(ni);
			continue;
		}

		SColor color(&ni->color.x);
		for (int a = 0; a < 3; ++a) {
			const float modage = age + a * 0.1f;
			const float rot = modage * 3;
			float size = baseSize - modage * baseSize * 0.9f;
			if (size < 0){
				if (size < -baseSize * 0.4f) {
					continue;
				} else if (size > -baseSize * 0.2f) {
					size = modage * baseSize * 0.9f - baseSize;
				} else {
					size = baseSize * 1.4f - modage * baseSize * 0.9f;
				}
			}
			color.a = (255 * ni->color[3]) / (3 - a);
			const float sinSize = fastmath::sin(rot) * size;
			const float cosSize = fastmath::cos(rot) * size;
			va->AddVertexC(float3(ni->pos.x + sinSize, ni->pos.z + cosSize, 0.0f),color);
			va->AddVertexC(float3(ni->pos.x + cosSize, ni->pos.z - sinSize, 0.0f),color);
			va->AddVertexC(float3(ni->pos.x + cosSize, ni->pos.z - sinSize, 0.0f),color);
			va->AddVertexC(float3(ni->pos.x - sinSize, ni->pos.z - cosSize, 0.0f),color);
			va->AddVertexC(float3(ni->pos.x - sinSize, ni->pos.z - cosSize, 0.0f),color);
			va->AddVertexC(float3(ni->pos.x - cosSize, ni->pos.z + sinSize, 0.0f),color);
			va->AddVertexC(float3(ni->pos.x - cosSize, ni->pos.z + sinSize, 0.0f),color);
			va->AddVertexC(float3(ni->pos.x + sinSize, ni->pos.z + cosSize, 0.0f),color);
		}
		++ni;
	}
	va->DrawArrayC(GL_LINES);
}



bool CMiniMap::RenderCachedTexture(bool useNormalizedCoors)
{
	glAttribStatePtr->PushColorBufferBit();
	glBindTexture(GL_TEXTURE_2D, minimapTextureID);
	glEnable(GL_TEXTURE_2D);
	glAttribStatePtr->DisableBlendMask();

	if (useNormalizedCoors)
		EnterNormalizedCoors(true, globalRendering->dualScreenMode);

	glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
	glBegin(GL_QUADS);
		glTexCoord2f(0.0, 0.0); glVertex2f(0.0f, 0.0f);
		glTexCoord2f(1.0, 0.0); glVertex2f(1.0f, 0.0f);
		glTexCoord2f(1.0, 1.0); glVertex2f(1.0f, 1.0f);
		glTexCoord2f(0.0, 1.0); glVertex2f(0.0f, 1.0f);
	glEnd();

	glAttribStatePtr->EnableBlendMask();
	glAttribStatePtr->BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	DrawCameraFrustumAndMouseSelection();
	LeaveNormalizedCoors(useNormalizedCoors, useNormalizedCoors && globalRendering->dualScreenMode);

	glDisable(GL_TEXTURE_2D);
	glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
	glAttribStatePtr->PopBits();
	return true;
}


void CMiniMap::DrawBackground() const
{
	glColor4f(0.6f, 0.6f, 0.6f, 1.0f);

	// don't mirror the map texture with flipped cameras
	GL::MatrixMode(GL_TEXTURE);
	GL::PushMatrix();
	GL::LoadIdentity();
	GL::MatrixMode(GL_MODELVIEW);

		// draw the map
		glAttribStatePtr->DisableAlphaTest();
		glAttribStatePtr->DisableBlendMask();
			readMap->DrawMinimap();
		glAttribStatePtr->EnableBlendMask();

	GL::MatrixMode(GL_TEXTURE);
	GL::PopMatrix();
	GL::MatrixMode(GL_MODELVIEW);

	glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
}


void CMiniMap::DrawUnitIcons() const
{
	// switch to top-down map/world coords (z is twisted with y compared to the real map/world coords)
	GL::PushMatrix();
	GL::Translate(0.0f, +1.0f, 0.0f);
	GL::Scale(+1.0f / (mapDims.mapx * SQUARE_SIZE), -1.0f / (mapDims.mapy * SQUARE_SIZE), 1.0f);

	glEnable(GL_TEXTURE_2D);
	glAttribStatePtr->EnableAlphaTest();
	glAttribStatePtr->AlphaFunc(GL_GREATER, 0.0f);

	unitDrawer->DrawUnitMiniMapIcons();

	glAttribStatePtr->DisableAlphaTest();
	glDisable(GL_TEXTURE_2D);

	GL::PopMatrix();
}


void CMiniMap::DrawUnitRanges() const
{
	// draw unit ranges
	const auto& selUnits = selectedUnitsHandler.selectedUnits;
	const float4 colors[] = {cmdColors.rangeInterceptorOff, cmdColors.rangeInterceptorOn};

	for (const int unitID: selUnits) {
		const CUnit* unit = unitHandler.GetUnit(unitID);

		// LOS Ranges
		if (unit->radarRadius && !unit->beingBuilt && unit->activated)
			DrawCircle(GetVertexArray(), {unit->pos, unit->radarRadius * 1.0f}, cmdColors.rangeRadar);

		if (unit->sonarRadius && !unit->beingBuilt && unit->activated)
			DrawCircle(GetVertexArray(), {unit->pos, unit->sonarRadius * 1.0f}, cmdColors.rangeSonar);

		if (unit->jammerRadius && !unit->beingBuilt && unit->activated)
			DrawCircle(GetVertexArray(), {unit->pos, unit->jammerRadius * 1.0f}, cmdColors.rangeJammer);

		// Interceptor Ranges
		for (const CWeapon* w: unit->weapons) {
			const WeaponDef& wd = *w->weaponDef;

			if ((w->range <= 300) || !wd.interceptor)
				continue;

			DrawCircle(GetVertexArray(), {unit->pos, wd.coverageRange}, colors[w->numStockpiled || !wd.stockpile]);
		}
	}
}


void CMiniMap::DrawWorldStuff() const
{
	GL::PushMatrix();
	GL::Translate(0.0f, +1.0f, 0.0f);
	GL::Scale(+1.0f / (mapDims.mapx * SQUARE_SIZE), -1.0f / (mapDims.mapy * SQUARE_SIZE), 1.0f);
	GL::RotateX(-90.0f); // real 'world' coordinates
	GL::Scale(1.0f, 0.0f, 1.0f); // skip the y-coord (Lua's DrawScreen is perspective and so any z-coord in it influence the x&y, too)

	// draw the projectiles
	if (drawProjectiles)
		projectileDrawer->DrawProjectilesMiniMap();

	{
		// draw the queued commands
		commandDrawer->DrawLuaQueuedUnitSetCommands();

		#if 0
		// TODO: pass the minimap transform (calls into CommandDrawer)
		// NOTE: this needlessly adds to the CursorIcons list, but at least
		//       they are not drawn  (because the input receivers are drawn
		//       after the command queues)
		if ((drawCommands > 0) && guihandler->GetQueueKeystate()) {
			selectedUnitsHandler.DrawCommands();
		}
		#endif
	}


	glAttribStatePtr->LineWidth(2.5f);
	lineDrawer.DrawAll(true);
	glAttribStatePtr->LineWidth(1.0f);

	// draw the selection shape, and some ranges
	if (drawCommands > 0)
		guihandler->DrawMapStuff(true);

	DrawUnitRanges();

	GL::PopMatrix();
}


void CMiniMap::SetClipPlanes(const bool lua) const
{
	if (lua) {
		// prepare ClipPlanes for Lua's DrawInMinimap Modelview matrix

		// quote from glClipPlane spec:
		// "When glClipPlane is called, equation is transformed by the inverse of the modelview matrix and stored in the resulting eye coordinates.
		//  Subsequent changes to the modelview matrix have no effect on the stored plane-equation components."
		// -> we have to use the same modelview matrix when calling glClipPlane and later draw calls

		// set the modelview matrix to the same as used in Lua's DrawInMinimap
		GL::PushMatrix();
		GL::LoadIdentity();
		GL::Scale(1.0f / curDim.x, 1.0f / curDim.y, 1.0f);

		const double plane0[4] = { 0, -1, 0, double(curDim.y)};
		const double plane1[4] = { 0,  1, 0,                0};
		const double plane2[4] = {-1,  0, 0, double(curDim.x)};
		const double plane3[4] = { 1,  0, 0,                0};

		glClipPlane(GL_CLIP_PLANE0, plane0); // clip bottom
		glClipPlane(GL_CLIP_PLANE1, plane1); // clip top
		glClipPlane(GL_CLIP_PLANE2, plane2); // clip right
		glClipPlane(GL_CLIP_PLANE3, plane3); // clip left

		GL::PopMatrix();
	} else {
		// clip everything outside of the minimap box
		const double plane0[4] = { 0,-1, 0, 1};
		const double plane1[4] = { 0, 1, 0, 0};
		const double plane2[4] = {-1, 0, 0, 1};
		const double plane3[4] = { 1, 0, 0, 0};

		glClipPlane(GL_CLIP_PLANE0, plane0); // clip bottom
		glClipPlane(GL_CLIP_PLANE1, plane1); // clip top
		glClipPlane(GL_CLIP_PLANE2, plane2); // clip right
		glClipPlane(GL_CLIP_PLANE3, plane3); // clip left
	}
}



/******************************************************************************/
