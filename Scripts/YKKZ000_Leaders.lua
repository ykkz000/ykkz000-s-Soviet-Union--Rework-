-----@param pPlayer Player
-----@param sLeaderType string
-----@return boolean
--function __ykkz000_su_check_leader(pPlayer, sLeaderType)
--    return PlayerConfigurations[pPlayer:GetID()]:GetLeaderTypeName() == sLeaderType;
--end

---@param pPlayer Player
---@param sTraitType string
---@return boolean
local function __ykkz000_has_trait(pPlayer, sTraitType)
    return (pPlayer:GetProperty('PROPERTY_' .. sTraitType) or 0) > 0;
end

---@param iKilledPlayerID number
---@param iKilledUnitID number
---@param iPlayerID number
---@param iUnitID number
function TRAIT_LEADER_YKKZ000_GRAND_DEPTH_OPERATIONAL_THEORY_KILL(iKilledPlayerID, iKilledUnitID, iPlayerID, iUnitID)
    local pPlayer = PlayerManager.GetPlayer(iPlayerID);
    if (pPlayer == nil) then
        return;
    end
    if (not __ykkz000_has_trait(pPlayer, "TRAIT_LEADER_YKKZ000_GRAND_DEPTH_OPERATIONAL_THEORY")) then
        return;
    end
    local pUnit = UnitManager.GetUnit(iPlayerID, iUnitID);
    UnitManager.RestoreUnitAttacks(pUnit);
    UnitManager.RestoreMovementToFormation(pUnit);
end

Events.UnitKilledInCombat.Add(TRAIT_LEADER_YKKZ000_GRAND_DEPTH_OPERATIONAL_THEORY_KILL);
