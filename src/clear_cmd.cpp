/* $Id$ */

/** @file clear_cmd.cpp */

#include "stdafx.h"
#include "openttd.h"
#include "clear_map.h"
#include "rail_map.h"
#include "table/strings.h"
#include "player.h"
#include "viewport.h"
#include "command_func.h"
#include "tunnel_map.h"
#include "bridge_map.h"
#include "bridge.h"
#include "landscape.h"
#include "variables.h"
#include "table/sprites.h"
#include "unmovable_map.h"
#include "genworld.h"
#include "industry.h"
#include "water_map.h"
#include "tile_cmd.h"
#include "functions.h"
#include "economy_func.h"
#include "vehicle_func.h"

/*
 * In one terraforming command all four corners of a initial tile can be raised/lowered (though this is not available to the player).
 * The maximal amount of height modifications is archieved when raising a complete flat land from sea level to MAX_TILE_HEIGHT or vice versa.
 * This affects all corners with a manhatten distance smaller than MAX_TILE_HEIGHT to one of the initial 4 corners.
 * Their maximal amount is computed to 4 * \sum_{i=1}^{h_max} i  =  2 * h_max * (h_max + 1).
 */
static const int TERRAFORMER_MODHEIGHT_SIZE = 2 * MAX_TILE_HEIGHT * (MAX_TILE_HEIGHT + 1);

/*
 * The maximal amount of affected tiles (i.e. the tiles that incident with one of the corners above, is computed similiar to
 * 1 + 4 * \sum_{i=1}^{h_max} (i+1)  =  1 + 2 * h_max + (h_max + 3).
 */
static const int TERRAFORMER_TILE_TABLE_SIZE = 1 + 2 * MAX_TILE_HEIGHT * (MAX_TILE_HEIGHT + 3);

struct TerraformerHeightMod {
	TileIndex tile;   ///< Referenced tile.
	byte height;      ///< New TileHeight (height of north corner) of the tile.
};

struct TerraformerState {
	int modheight_count;                                         ///< amount of entries in "modheight".
	int tile_table_count;                                        ///< amount of entries in "tile_table".

	/**
	 * Dirty tiles, i.e.\ at least one corner changed.
	 *
	 * This array contains the tiles which are or will be marked as dirty.
	 *
	 * @ingroup dirty
	 */
	TileIndex tile_table[TERRAFORMER_TILE_TABLE_SIZE];
	TerraformerHeightMod modheight[TERRAFORMER_MODHEIGHT_SIZE];  ///< Height modifications.
};

/**
 * Gets the TileHeight (height of north corner) of a tile as of current terraforming progress.
 *
 * @param ts TerraformerState.
 * @param tile Tile.
 * @return TileHeight.
 */
static int TerraformGetHeightOfTile(TerraformerState *ts, TileIndex tile)
{
	TerraformerHeightMod *mod = ts->modheight;
	int count;

	for (count = ts->modheight_count; count != 0; count--, mod++) {
		if (mod->tile == tile) return mod->height;
	}

	/* TileHeight unchanged so far, read value from map. */
	return TileHeight(tile);
}

/**
 * Stores the TileHeight (height of north corner) of a tile in a TerraformerState.
 *
 * @param ts TerraformerState.
 * @param tile Tile.
 * @param height New TileHeight.
 */
static void TerraformSetHeightOfTile(TerraformerState *ts, TileIndex tile, int height)
{
	/* Find tile in the "modheight" table.
	 * Note: In a normal user-terraform command the tile will not be found in the "modheight" table.
	 *       But during house- or industry-construction multiple corners can be terraformed at once. */
	TerraformerHeightMod *mod = ts->modheight;
	int count = ts->modheight_count;
	while ((count > 0) && (mod->tile != tile)) {
		mod++;
		count--;
	}

	/* New entry? */
	if (count == 0) {
		assert(ts->modheight_count < TERRAFORMER_MODHEIGHT_SIZE);
		ts->modheight_count++;
	}

	/* Finally store the new value */
	mod->tile = tile;
	mod->height = (byte)height;
}

/**
 * Adds a tile to the "tile_table" in a TerraformerState.
 *
 * @param ts TerraformerState.
 * @param tile Tile.
 * @ingroup dirty
 */
static void TerraformAddDirtyTile(TerraformerState *ts, TileIndex tile)
{
	int count;
	TileIndex *t;

	count = ts->tile_table_count;

	for (t = ts->tile_table; count != 0; count--, t++) {
		if (*t == tile) return;
	}

	assert(ts->tile_table_count < TERRAFORMER_TILE_TABLE_SIZE);

	ts->tile_table[ts->tile_table_count++] = tile;
}

/**
 * Adds all tiles that incident with the north corner of a specific tile to the "tile_table" in a TerraformerState.
 *
 * @param ts TerraformerState.
 * @param tile Tile.
 * @ingroup dirty
 */
static void TerraformAddDirtyTileAround(TerraformerState *ts, TileIndex tile)
{
	TerraformAddDirtyTile(ts, tile + TileDiffXY( 0, -1));
	TerraformAddDirtyTile(ts, tile + TileDiffXY(-1, -1));
	TerraformAddDirtyTile(ts, tile + TileDiffXY(-1,  0));
	TerraformAddDirtyTile(ts, tile);
}

/**
 * Terraform the north corner of a tile to a specific height.
 *
 * @param ts TerraformerState.
 * @param tile Tile.
 * @param height Aimed height.
 * @param return Error code or cost.
 */
static CommandCost TerraformTileHeight(TerraformerState *ts, TileIndex tile, int height)
{
	CommandCost total_cost = CommandCost();

	assert(tile < MapSize());

	/* Check range of destination height */
	if (height < 0) return_cmd_error(STR_1003_ALREADY_AT_SEA_LEVEL);
	if (height > MAX_TILE_HEIGHT) return_cmd_error(STR_1004_TOO_HIGH);

	/*
	 * Check if the terraforming has any effect.
	 * This can only be true, if multiple corners of the start-tile are terraformed (i.e. the terraforming is done by towns/industries etc.).
	 * In this case the terraforming should fail. (Don't know why.)
	 */
	if (height == TerraformGetHeightOfTile(ts, tile)) return CMD_ERROR;

	/* Check "too close to edge of map" */
	uint x = TileX(tile);
	uint y = TileY(tile);
	if ((x <= 1) || (y <= 1) || (x >= MapMaxX() - 1) || (y >= MapMaxY() - 1)) {
		/*
		 * Determine a sensible error tile
		 * Note: If x and y are both zero this will disable the error tile. (Tile 0 cannot be highlighted :( )
		 */
		if ((x == 1) && (y != 0)) x = 0;
		if ((y == 1) && (x != 0)) y = 0;
		_terraform_err_tile = TileXY(x, y);
		return_cmd_error(STR_0002_TOO_CLOSE_TO_EDGE_OF_MAP);
	}

	/* Mark incident tiles, that are involved in the terraforming */
	TerraformAddDirtyTileAround(ts, tile);

	/* Store the height modification */
	TerraformSetHeightOfTile(ts, tile, height);

	/* Increment cost */
	total_cost.AddCost(_price.terraform);

	/* Recurse to neighboured corners if height difference is larger than 1 */
	{
		const TileIndexDiffC *ttm;

		static const TileIndexDiffC _terraform_tilepos[] = {
			{ 1,  0}, // move to tile in SE
			{-2,  0}, // undo last move, and move to tile in NW
			{ 1,  1}, // undo last move, and move to tile in SW
			{ 0, -2}  // undo last move, and move to tile in NE
		};

		for (ttm = _terraform_tilepos; ttm != endof(_terraform_tilepos); ttm++) {
			tile += ToTileIndexDiff(*ttm);

			/* Get TileHeight of neighboured tile as of current terraform progress */
			int r = TerraformGetHeightOfTile(ts, tile);
			int height_diff = height - r;

			/* Is the height difference to the neighboured corner greater than 1? */
			if (abs(height_diff) > 1) {
				/* Terraform the neighboured corner. The resulting height difference should be 1. */
				height_diff += (height_diff < 0 ? 1 : -1);
				CommandCost cost = TerraformTileHeight(ts, tile, r + height_diff);
				if (CmdFailed(cost)) return cost;
				total_cost.AddCost(cost);
			}
		}
	}

	return total_cost;
}

/** Terraform land
 * @param tile tile to terraform
 * @param flags for this command type
 * @param p1 corners to terraform (SLOPE_xxx)
 * @param p2 direction; eg up (non-zero) or down (zero)
 * @return error or cost of terraforming
 */
CommandCost CmdTerraformLand(TileIndex tile, uint32 flags, uint32 p1, uint32 p2)
{
	TerraformerState ts;
	CommandCost total_cost = CommandCost();
	int direction = (p2 != 0 ? 1 : -1);

	SET_EXPENSES_TYPE(EXPENSES_CONSTRUCTION);

	_terraform_err_tile = 0;

	ts.modheight_count = ts.tile_table_count = 0;

	/* Make an extra check for map-bounds cause we add tiles to the originating tile */
	if (tile + TileDiffXY(1, 1) >= MapSize()) return CMD_ERROR;

	/* Compute the costs and the terraforming result in a model of the landscape */
	if ((p1 & SLOPE_W) != 0) {
		TileIndex t = tile + TileDiffXY(1, 0);
		CommandCost cost = TerraformTileHeight(&ts, t, TileHeight(t) + direction);
		if (CmdFailed(cost)) return cost;
		total_cost.AddCost(cost);
	}

	if ((p1 & SLOPE_S) != 0) {
		TileIndex t = tile + TileDiffXY(1, 1);
		CommandCost cost = TerraformTileHeight(&ts, t, TileHeight(t) + direction);
		if (CmdFailed(cost)) return cost;
		total_cost.AddCost(cost);
	}

	if ((p1 & SLOPE_E) != 0) {
		TileIndex t = tile + TileDiffXY(0, 1);
		CommandCost cost = TerraformTileHeight(&ts, t, TileHeight(t) + direction);
		if (CmdFailed(cost)) return cost;
		total_cost.AddCost(cost);
	}

	if ((p1 & SLOPE_N) != 0) {
		TileIndex t = tile + TileDiffXY(0, 0);
		CommandCost cost = TerraformTileHeight(&ts, t, TileHeight(t) + direction);
		if (CmdFailed(cost)) return cost;
		total_cost.AddCost(cost);
	}

	/* Check if the terraforming is valid wrt. tunnels, bridges and objects on the surface */
	{
		int count;
		TileIndex *ti = ts.tile_table;

		for (count = ts.tile_table_count; count != 0; count--, ti++) {
			TileIndex tile = *ti;

			/* Find new heights of tile corners */
			uint z_N = TerraformGetHeightOfTile(&ts, tile + TileDiffXY(0, 0));
			uint z_W = TerraformGetHeightOfTile(&ts, tile + TileDiffXY(1, 0));
			uint z_S = TerraformGetHeightOfTile(&ts, tile + TileDiffXY(1, 1));
			uint z_E = TerraformGetHeightOfTile(&ts, tile + TileDiffXY(0, 1));

			/* Find min and max height of tile */
			uint z_min = min(min(z_N, z_W), min(z_S, z_E));
			uint z_max = max(max(z_N, z_W), max(z_S, z_E));

			/* Compute tile slope */
			uint tileh = (z_max > z_min + 1 ? SLOPE_STEEP : SLOPE_FLAT);
			if (z_W > z_min) tileh += SLOPE_W;
			if (z_S > z_min) tileh += SLOPE_S;
			if (z_E > z_min) tileh += SLOPE_E;
			if (z_N > z_min) tileh += SLOPE_N;

			/* Check if bridge would take damage */
			if (direction == 1 && MayHaveBridgeAbove(tile) && IsBridgeAbove(tile) &&
					GetBridgeHeight(GetSouthernBridgeEnd(tile)) <= z_max * TILE_HEIGHT) {
				_terraform_err_tile = tile; // highlight the tile under the bridge
				return_cmd_error(STR_5007_MUST_DEMOLISH_BRIDGE_FIRST);
			}
			/* Check if tunnel would take damage */
			if (direction == -1 && IsTunnelInWay(tile, z_min * TILE_HEIGHT)) {
				_terraform_err_tile = tile; // highlight the tile above the tunnel
				return_cmd_error(STR_1002_EXCAVATION_WOULD_DAMAGE);
			}
			/* Check tiletype-specific things, and add extra-cost */
			CommandCost cost = _tile_type_procs[GetTileType(tile)]->terraform_tile_proc(tile, flags | DC_AUTO, z_min * TILE_HEIGHT, (Slope) tileh);
			if (CmdFailed(cost)) {
				_terraform_err_tile = tile;
				return cost;
			}
			total_cost.AddCost(cost);
		}
	}

	if (flags & DC_EXEC) {
		/* change the height */
		{
			int count;
			TerraformerHeightMod *mod;

			mod = ts.modheight;
			for (count = ts.modheight_count; count != 0; count--, mod++) {
				TileIndex til = mod->tile;

				SetTileHeight(til, mod->height);
			}
		}

		/* finally mark the dirty tiles dirty */
		{
			int count;
			TileIndex *ti = ts.tile_table;
			for (count = ts.tile_table_count; count != 0; count--, ti++) {
				MarkTileDirtyByTile(*ti);
			}
		}
	}
	return total_cost;
}


/** Levels a selected (rectangle) area of land
 * @param tile end tile of area-drag
 * @param flags for this command type
 * @param p1 start tile of area drag
 * @param p2 height difference; eg raise (+1), lower (-1) or level (0)
 * @return  error or cost of terraforming
 */
CommandCost CmdLevelLand(TileIndex tile, uint32 flags, uint32 p1, uint32 p2)
{
	int size_x, size_y;
	int ex;
	int ey;
	int sx, sy;
	uint h, oldh, curh;
	CommandCost money;
	CommandCost ret;
	CommandCost cost;

	if (p1 >= MapSize()) return CMD_ERROR;

	SET_EXPENSES_TYPE(EXPENSES_CONSTRUCTION);

	/* remember level height */
	oldh = TileHeight(p1);

	/* compute new height */
	h = oldh + p2;

	/* Check range of destination height */
	if (h > MAX_TILE_HEIGHT) return_cmd_error((oldh == 0) ? STR_1003_ALREADY_AT_SEA_LEVEL : STR_1004_TOO_HIGH);

	/* make sure sx,sy are smaller than ex,ey */
	ex = TileX(tile);
	ey = TileY(tile);
	sx = TileX(p1);
	sy = TileY(p1);
	if (ex < sx) Swap(ex, sx);
	if (ey < sy) Swap(ey, sy);
	tile = TileXY(sx, sy);

	size_x = ex - sx + 1;
	size_y = ey - sy + 1;

	money.AddCost(GetAvailableMoneyForCommand());

	BEGIN_TILE_LOOP(tile2, size_x, size_y, tile) {
		curh = TileHeight(tile2);
		while (curh != h) {
			ret = DoCommand(tile2, SLOPE_N, (curh > h) ? 0 : 1, flags & ~DC_EXEC, CMD_TERRAFORM_LAND);
			if (CmdFailed(ret)) break;

			if (flags & DC_EXEC) {
				money.AddCost(-ret.GetCost());
				if (money.GetCost() < 0) {
					_additional_cash_required = ret.GetCost();
					return cost;
				}
				DoCommand(tile2, SLOPE_N, (curh > h) ? 0 : 1, flags, CMD_TERRAFORM_LAND);
			}

			cost.AddCost(ret);
			curh += (curh > h) ? -1 : 1;
		}
	} END_TILE_LOOP(tile2, size_x, size_y, tile)

	return (cost.GetCost() == 0) ? CMD_ERROR : cost;
}

/** Purchase a land area. Actually you only purchase one tile, so
 * the name is a bit confusing ;p
 * @param tile the tile the player is purchasing
 * @param flags for this command type
 * @param p1 unused
 * @param p2 unused
 * @return error of cost of operation
 */
CommandCost CmdPurchaseLandArea(TileIndex tile, uint32 flags, uint32 p1, uint32 p2)
{
	CommandCost cost;

	SET_EXPENSES_TYPE(EXPENSES_CONSTRUCTION);

	if (IsOwnedLandTile(tile) && IsTileOwner(tile, _current_player)) {
		return_cmd_error(STR_5807_YOU_ALREADY_OWN_IT);
	}

	cost = DoCommand(tile, 0, 0, flags, CMD_LANDSCAPE_CLEAR);
	if (CmdFailed(cost)) return CMD_ERROR;

	if (flags & DC_EXEC) {
		MakeOwnedLand(tile, _current_player);
		MarkTileDirtyByTile(tile);
	}

	return cost.AddCost(_price.clear_roughland * 10);
}


static CommandCost ClearTile_Clear(TileIndex tile, byte flags)
{
	static const Money* clear_price_table[] = {
		&_price.clear_grass,
		&_price.clear_roughland,
		&_price.clear_rocks,
		&_price.clear_fields,
		&_price.clear_roughland,
		&_price.clear_roughland,
	};
	CommandCost price;

	if (!IsClearGround(tile, CLEAR_GRASS) || GetClearDensity(tile) != 0) {
		price.AddCost(*clear_price_table[GetClearGround(tile)]);
	}

	if (flags & DC_EXEC) DoClearSquare(tile);

	return price;
}

/** Sell a land area. Actually you only sell one tile, so
 * the name is a bit confusing ;p
 * @param tile the tile the player is selling
 * @param flags for this command type
 * @param p1 unused
 * @param p2 unused
 * @return error or cost of operation
 */
CommandCost CmdSellLandArea(TileIndex tile, uint32 flags, uint32 p1, uint32 p2)
{
	SET_EXPENSES_TYPE(EXPENSES_CONSTRUCTION);

	if (!IsOwnedLandTile(tile)) return CMD_ERROR;
	if (!CheckTileOwnership(tile) && _current_player != OWNER_WATER) return CMD_ERROR;


	if (!EnsureNoVehicleOnGround(tile)) return CMD_ERROR;

	if (flags & DC_EXEC) DoClearSquare(tile);

	return CommandCost(- _price.clear_roughland * 2);
}


#include "table/clear_land.h"


void DrawClearLandTile(const TileInfo *ti, byte set)
{
	DrawGroundSprite(SPR_FLAT_BARE_LAND + _tileh_to_sprite[ti->tileh] + set * 19, PAL_NONE);
}

void DrawHillyLandTile(const TileInfo *ti)
{
	if (ti->tileh != SLOPE_FLAT) {
		DrawGroundSprite(SPR_FLAT_ROUGH_LAND + _tileh_to_sprite[ti->tileh], PAL_NONE);
	} else {
		DrawGroundSprite(_landscape_clear_sprites[GB(ti->x ^ ti->y, 4, 3)], PAL_NONE);
	}
}

void DrawClearLandFence(const TileInfo *ti)
{
	byte z = ti->z;

	if (ti->tileh & SLOPE_S) {
		z += TILE_HEIGHT;
		if (ti->tileh == SLOPE_STEEP_S) z += TILE_HEIGHT;
	}

	if (GetFenceSW(ti->tile) != 0) {
		DrawGroundSpriteAt(_clear_land_fence_sprites_1[GetFenceSW(ti->tile) - 1] + _fence_mod_by_tileh[ti->tileh], PAL_NONE, ti->x, ti->y, z);
	}

	if (GetFenceSE(ti->tile) != 0) {
		DrawGroundSpriteAt(_clear_land_fence_sprites_1[GetFenceSE(ti->tile) - 1] + _fence_mod_by_tileh_2[ti->tileh], PAL_NONE, ti->x, ti->y, z);
	}
}

static void DrawTile_Clear(TileInfo *ti)
{
	switch (GetClearGround(ti->tile)) {
		case CLEAR_GRASS:
			DrawClearLandTile(ti, GetClearDensity(ti->tile));
			break;

		case CLEAR_ROUGH:
			DrawHillyLandTile(ti);
			break;

		case CLEAR_ROCKS:
			DrawGroundSprite(SPR_FLAT_ROCKY_LAND_1 + _tileh_to_sprite[ti->tileh], PAL_NONE);
			break;

		case CLEAR_FIELDS:
			DrawGroundSprite(_clear_land_sprites_1[GetFieldType(ti->tile)] + _tileh_to_sprite[ti->tileh], PAL_NONE);
			break;

		case CLEAR_SNOW:
			DrawGroundSprite(_clear_land_sprites_2[GetClearDensity(ti->tile)] + _tileh_to_sprite[ti->tileh], PAL_NONE);
			break;

		case CLEAR_DESERT:
			DrawGroundSprite(_clear_land_sprites_3[GetClearDensity(ti->tile)] + _tileh_to_sprite[ti->tileh], PAL_NONE);
			break;
	}

	DrawClearLandFence(ti);
	DrawBridgeMiddle(ti);
}

static uint GetSlopeZ_Clear(TileIndex tile, uint x, uint y)
{
	uint z;
	Slope tileh = GetTileSlope(tile, &z);

	return z + GetPartialZ(x & 0xF, y & 0xF, tileh);
}

static Foundation GetFoundation_Clear(TileIndex tile, Slope tileh)
{
	return FOUNDATION_NONE;
}

static void GetAcceptedCargo_Clear(TileIndex tile, AcceptedCargo ac)
{
	/* unused */
}

static void AnimateTile_Clear(TileIndex tile)
{
	/* unused */
}

void TileLoopClearHelper(TileIndex tile)
{
	byte self;
	byte neighbour;
	TileIndex dirty = INVALID_TILE;

	self = (IsTileType(tile, MP_CLEAR) && IsClearGround(tile, CLEAR_FIELDS));

	neighbour = (IsTileType(TILE_ADDXY(tile, 1, 0), MP_CLEAR) && IsClearGround(TILE_ADDXY(tile, 1, 0), CLEAR_FIELDS));
	if (GetFenceSW(tile) == 0) {
		if (self != neighbour) {
			SetFenceSW(tile, 3);
			dirty = tile;
		}
	} else {
		if (self == 0 && neighbour == 0) {
			SetFenceSW(tile, 0);
			dirty = tile;
		}
	}

	neighbour = (IsTileType(TILE_ADDXY(tile, 0, 1), MP_CLEAR) && IsClearGround(TILE_ADDXY(tile, 0, 1), CLEAR_FIELDS));
	if (GetFenceSE(tile) == 0) {
		if (self != neighbour) {
			SetFenceSE(tile, 3);
			dirty = tile;
		}
	} else {
		if (self == 0 && neighbour == 0) {
			SetFenceSE(tile, 0);
			dirty = tile;
		}
	}

	if (dirty != INVALID_TILE) MarkTileDirtyByTile(dirty);
}


/* convert into snowy tiles */
static void TileLoopClearAlps(TileIndex tile)
{
	int k = GetTileZ(tile) - GetSnowLine() + TILE_HEIGHT;

	if (k < 0) { // well below the snow line
		if (!IsClearGround(tile, CLEAR_SNOW)) return;
		if (GetClearDensity(tile) == 0) SetClearGroundDensity(tile, CLEAR_GRASS, 3);
	} else {
		if (!IsClearGround(tile, CLEAR_SNOW)) {
			SetClearGroundDensity(tile, CLEAR_SNOW, 0);
		} else {
			uint density = min((uint)k / TILE_HEIGHT, 3);

			if (GetClearDensity(tile) < density) {
				AddClearDensity(tile, 1);
			} else if (GetClearDensity(tile) > density) {
				AddClearDensity(tile, -1);
			} else {
				return;
			}
		}
	}

	MarkTileDirtyByTile(tile);
}

static void TileLoopClearDesert(TileIndex tile)
{
	if (IsClearGround(tile, CLEAR_DESERT)) return;

	if (GetTropicZone(tile) == TROPICZONE_DESERT) {
		SetClearGroundDensity(tile, CLEAR_DESERT, 3);
	} else {
		if (GetTropicZone(tile + TileDiffXY( 1,  0)) != TROPICZONE_DESERT &&
				GetTropicZone(tile + TileDiffXY(-1,  0)) != TROPICZONE_DESERT &&
				GetTropicZone(tile + TileDiffXY( 0,  1)) != TROPICZONE_DESERT &&
				GetTropicZone(tile + TileDiffXY( 0, -1)) != TROPICZONE_DESERT)
			return;
		SetClearGroundDensity(tile, CLEAR_DESERT, 1);
	}

	MarkTileDirtyByTile(tile);
}

static void TileLoop_Clear(TileIndex tile)
{
	TileLoopClearHelper(tile);

	switch (_opt.landscape) {
		case LT_TROPIC: TileLoopClearDesert(tile); break;
		case LT_ARCTIC: TileLoopClearAlps(tile);   break;
	}

	switch (GetClearGround(tile)) {
		case CLEAR_GRASS:
			if (GetClearDensity(tile) == 3) return;

			if (_game_mode != GM_EDITOR) {
				if (GetClearCounter(tile) < 7) {
					AddClearCounter(tile, 1);
					return;
				} else {
					SetClearCounter(tile, 0);
					AddClearDensity(tile, 1);
				}
			} else {
				SetClearGroundDensity(tile, GB(Random(), 0, 8) > 21 ? CLEAR_GRASS : CLEAR_ROUGH, 3);
			}
			break;

		case CLEAR_FIELDS: {
			uint field_type;

			if (_game_mode == GM_EDITOR) return;

			if (GetClearCounter(tile) < 7) {
				AddClearCounter(tile, 1);
				return;
			} else {
				SetClearCounter(tile, 0);
			}

			if (GetIndustryIndexOfField(tile) == INVALID_INDUSTRY && GetFieldType(tile) >= 7) {
				/* This farmfield is no longer farmfield, so make it grass again */
				MakeClear(tile, CLEAR_GRASS, 2);
			} else {
				field_type = GetFieldType(tile);
				field_type = (field_type < 8) ? field_type + 1 : 0;
				SetFieldType(tile, field_type);
			}
			break;
		}

		default:
			return;
	}

	MarkTileDirtyByTile(tile);
}

void GenerateClearTile()
{
	uint i, gi;
	TileIndex tile;

	/* add rough tiles */
	i = ScaleByMapSize(GB(Random(), 0, 10) + 0x400);
	gi = ScaleByMapSize(GB(Random(), 0, 7) + 0x80);

	SetGeneratingWorldProgress(GWP_ROUGH_ROCKY, gi + i);
	do {
		IncreaseGeneratingWorldProgress(GWP_ROUGH_ROCKY);
		tile = RandomTile();
		if (IsTileType(tile, MP_CLEAR) && !IsClearGround(tile, CLEAR_DESERT)) SetClearGroundDensity(tile, CLEAR_ROUGH, 3);
	} while (--i);

	/* add rocky tiles */
	i = gi;
	do {
		uint32 r = Random();
		tile = RandomTileSeed(r);

		IncreaseGeneratingWorldProgress(GWP_ROUGH_ROCKY);
		if (IsTileType(tile, MP_CLEAR) && !IsClearGround(tile, CLEAR_DESERT)) {
			uint j = GB(r, 16, 4) + 5;
			for (;;) {
				TileIndex tile_new;

				SetClearGroundDensity(tile, CLEAR_ROCKS, 3);
				do {
					if (--j == 0) goto get_out;
					tile_new = tile + TileOffsByDiagDir((DiagDirection)GB(Random(), 0, 2));
				} while (!IsTileType(tile_new, MP_CLEAR) || IsClearGround(tile_new, CLEAR_DESERT));
				tile = tile_new;
			}
get_out:;
		}
	} while (--i);
}

static void ClickTile_Clear(TileIndex tile)
{
	/* not used */
}

static uint32 GetTileTrackStatus_Clear(TileIndex tile, TransportType mode, uint sub_mode)
{
	return 0;
}

static const StringID _clear_land_str[] = {
	STR_080D_GRASS,
	STR_080B_ROUGH_LAND,
	STR_080A_ROCKS,
	STR_080E_FIELDS,
	STR_080F_SNOW_COVERED_LAND,
	STR_0810_DESERT
};

static void GetTileDesc_Clear(TileIndex tile, TileDesc *td)
{
	if (IsClearGround(tile, CLEAR_GRASS) && GetClearDensity(tile) == 0) {
		td->str = STR_080C_BARE_LAND;
	} else {
		td->str = _clear_land_str[GetClearGround(tile)];
	}
	td->owner = GetTileOwner(tile);
}

static void ChangeTileOwner_Clear(TileIndex tile, PlayerID old_player, PlayerID new_player)
{
	return;
}

void InitializeClearLand()
{
	_opt.snow_line = _patches.snow_line_height * TILE_HEIGHT;
}

static CommandCost TerraformTile_Clear(TileIndex tile, uint32 flags, uint z_new, Slope tileh_new)
{
	return DoCommand(tile, 0, 0, flags, CMD_LANDSCAPE_CLEAR);
}

extern const TileTypeProcs _tile_type_clear_procs = {
	DrawTile_Clear,           ///< draw_tile_proc
	GetSlopeZ_Clear,          ///< get_slope_z_proc
	ClearTile_Clear,          ///< clear_tile_proc
	GetAcceptedCargo_Clear,   ///< get_accepted_cargo_proc
	GetTileDesc_Clear,        ///< get_tile_desc_proc
	GetTileTrackStatus_Clear, ///< get_tile_track_status_proc
	ClickTile_Clear,          ///< click_tile_proc
	AnimateTile_Clear,        ///< animate_tile_proc
	TileLoop_Clear,           ///< tile_loop_clear
	ChangeTileOwner_Clear,    ///< change_tile_owner_clear
	NULL,                     ///< get_produced_cargo_proc
	NULL,                     ///< vehicle_enter_tile_proc
	GetFoundation_Clear,      ///< get_foundation_proc
	TerraformTile_Clear,      ///< terraform_tile_proc
};
