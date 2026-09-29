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
    if (pUnit ~= nil) then
        local pUnitInfo = GameInfo.Units[pUnit:GetType()];
        if (pUnitInfo ~= nil and pUnitInfo.PromotionClass == 'PROMOTION_CLASS_MELEE') then
            UnitManager.RestoreUnitAttacks(pUnit);
            UnitManager.RestoreMovementToFormation(pUnit);
        end
    end
    -- Player-wide: every 3 kills grants 1 free envoy; the kill count is stored on the player.
    local iKills = (pPlayer:GetProperty('PROPERTY_YKKZ000_STALIN_KILL_COUNT') or 0) + 1;
    if (iKills >= 3) then
        iKills = iKills - 3;
        pPlayer:AttachModifierByID('YKKZ000_STALIN_ENVOY_REWARD');
    end
    pPlayer:SetProperty('PROPERTY_YKKZ000_STALIN_KILL_COUNT', iKills);
end

Events.UnitKilledInCombat.Add(TRAIT_LEADER_YKKZ000_GRAND_DEPTH_OPERATIONAL_THEORY_KILL);

-- 为每位玩家登记其文明/领袖特性属性（供 __ykkz000_has_trait 查询）。
-- 必须在 LoadGameViewStateDone 时执行：文件加载阶段 PlayerConfigurations 尚未就绪，
-- 直接索引会得到 nil 并使整个脚本文件加载失败（进而连属性初始化也一并丢失）。
-- 逐项判空以避免个别玩家配置缺失时中断其余玩家的登记。
function YKKZ000_INITIALIZE_PLAYER_TRAITS()
    for _, pPlayer in ipairs(Players) do
        if (pPlayer ~= nil) then
            local pPlayerConfig = PlayerConfigurations[pPlayer:GetID()];
            if (pPlayerConfig ~= nil) then
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
        end
    end
end

Events.LoadGameViewStateDone.Add(YKKZ000_INITIALIZE_PLAYER_TRAITS);
