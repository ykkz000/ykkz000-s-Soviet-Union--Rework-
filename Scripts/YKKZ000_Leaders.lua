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
    return (pPlayer:GetProperty('PROPERTY_YKKZ000_' .. sTraitType) or 0) > 0;
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
    if (not __ykkz000_has_trait(pPlayer, 'TRAIT_LEADER_YKKZ000_GRAND_DEPTH_OPERATIONAL_THEORY')) then
        return;
    end
    local pUnit = UnitManager.GetUnit(iPlayerID, iUnitID);
    if (not GameInfo.Units[pUnit:GetType()].PromotionClass == 'PROMOTION_CLASS_MELEE') then
        return;
    end
    UnitManager.RestoreUnitAttacks(pUnit);
    UnitManager.RestoreMovementToFormation(pUnit);
end

Events.UnitKilledInCombat.Add(TRAIT_LEADER_YKKZ000_GRAND_DEPTH_OPERATIONAL_THEORY_KILL);

for _, pPlayer in ipairs(Players) do
    local pPlayerConfig = PlayerConfigurations[pPlayer:GetID()];
    local sCivilizationType = pPlayerConfig:GetCivilizationTypeName();
    local sLeaderType = pPlayerConfig:GetLeaderTypeName();
    for row in GameInfo.CivilizationTraits() do
        if (row.CivilizationType == sCivilizationType) then
            pPlayer:SetProperty('PROPERTY_YKKZ000_' .. row.TraitType, 1);
        end
    end
    for row in GameInfo.LeaderTraits() do
        if (row.LeaderType == sLeaderType) then
            pPlayer:SetProperty('PROPERTY_YKKZ000_' .. row.TraitType, 1);
        end
    end
end
