-----@param pPlayer Player
-----@param sCivilizationType string
-----@return boolean
--function __ykkz000_su_check_civilization(pPlayer, sCivilizationType)
--    return PlayerConfigurations[pPlayer:GetID()]:GetCivilizationTypeName() == sCivilizationType;
--end

---@param pPlayer Player
---@param sTraitType string
---@return boolean
local function __ykkz000_has_trait(pPlayer, sTraitType)
    return (pPlayer:GetProperty('PROPERTY_YKKZ000_' .. sTraitType) or 0) > 0;
end

---@param pCity City
function __ykkz000_su_city_production_adjust(pCity)
    local pPlayerCityPopulationTable = Game:GetProperty("YKKZ000_SU_PLAYER_CITY_POPULATION_TABLE");
    if (type(pPlayerCityPopulationTable) ~= "table") then
        pPlayerCityPopulationTable = {};
    end
    -- Update Table
    local pCityPopulationTable = pPlayerCityPopulationTable[pCity:GetOwner()];
    if (type(pCityPopulationTable) ~= "table") then
        pCityPopulationTable = {};
    end
    if (type(pCityPopulationTable[pCity:GetID()]) ~= "number") then
        pCityPopulationTable[pCity:GetID()] = 0;
    end
    local iOldPopulation = pCityPopulationTable[pCity:GetID()];
    local iNewPopulation = pCity:GetPopulation();
    pCityPopulationTable[pCity:GetID()] = iNewPopulation;
    pPlayerCityPopulationTable[pCity:GetOwner()] = pCityPopulationTable;
    Game:SetProperty("YKKZ000_SU_PLAYER_CITY_POPULATION_TABLE", pPlayerCityPopulationTable);
    -- Calculate Adjusting
    local iAdjustProduction = iNewPopulation - iOldPopulation;
    if (iAdjustProduction > 0) then
        for _ = 1, iAdjustProduction, 1 do
            pCity:AttachModifierByID("TRAIT_YKKZ000_FROM_ABILITY_TO_WORK_ADJUST_PRODUCTION_POSITIVE");
        end
    elseif (iAdjustProduction < 0) then
        for _ = 1, math.abs(iAdjustProduction), 1 do
            pCity:AttachModifierByID("TRAIT_YKKZ000_FROM_ABILITY_TO_WORK_ADJUST_PRODUCTION_NEGATIVE");
        end
    end
end

---@param iCityOwner number
---@param iCityID number
---@param iX number
---@param iY number
function TRAIT_LEADER_YKKZ000_FROM_ABILITY_TO_WORK_CITY_BUILD_PRODUCTION(iCityOwner, iCityID, iX, iY)
    local pPlayer = PlayerManager.GetPlayer(iCityOwner);
    if (pPlayer == nil) then
        return;
    end
    if (not __ykkz000_has_trait(pPlayer, "TRAIT_CIVILIZATION_YKKZ000_FROM_ABILITY_TO_WORK")) then
        return;
    end
    local pCity = CityManager.GetCity(iCityOwner, iCityID);
    __ykkz000_su_city_production_adjust(pCity);
end

---@param iCityOwner number
---@param iCityID number
---@param iAmountChanged number
function TRAIT_LEADER_YKKZ000_FROM_ABILITY_TO_WORK_POPULATION_CHANGE_PRODUCTION(iCityOwner, iCityID, iAmountChanged)
    local pPlayer = PlayerManager.GetPlayer(iCityOwner);
    if (pPlayer == nil) then
        return;
    end
    if (not __ykkz000_has_trait(pPlayer, "TRAIT_CIVILIZATION_YKKZ000_FROM_ABILITY_TO_WORK")) then
        return;
    end
    local pCity = CityManager.GetCity(iCityOwner, iCityID);
    __ykkz000_su_city_production_adjust(pCity);
end

GameEvents.CityBuilt.Add(TRAIT_LEADER_YKKZ000_FROM_ABILITY_TO_WORK_CITY_BUILD_PRODUCTION);
GameEvents.OnCityPopulationChanged.Add(TRAIT_LEADER_YKKZ000_FROM_ABILITY_TO_WORK_POPULATION_CHANGE_PRODUCTION);

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

