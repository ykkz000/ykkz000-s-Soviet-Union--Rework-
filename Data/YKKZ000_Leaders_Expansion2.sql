-- YKKZ000_Leaders_Expansion2
-- Author: ykkz000
-- DateCreated: 9/25/2026
--------------------------------------------------------------
-- Gathering Storm only: +100% Diplomatic Favor from Suzerain city-states
INSERT OR IGNORE INTO Modifiers (ModifierId, ModifierType)
VALUES ('TRAIT_YKKZ000_PEACE_DECREE_SUZERAIN_FAVOR', 'MODIFIER_PLAYER_ADJUST_SUZERAIN_FAVOR_MULTIPLIER');

INSERT OR IGNORE INTO ModifierArguments (ModifierId, Name, Value) VALUES
  ('TRAIT_YKKZ000_PEACE_DECREE_SUZERAIN_FAVOR', 'Amount', '100');

INSERT OR IGNORE INTO TraitModifiers (TraitType, ModifierId)
VALUES ('TRAIT_LEADER_YKKZ000_PEACE_DECREE', 'TRAIT_YKKZ000_PEACE_DECREE_SUZERAIN_FAVOR');
