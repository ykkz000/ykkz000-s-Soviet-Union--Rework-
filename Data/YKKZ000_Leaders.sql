-- YKKZ000_Leaders
-- Author: ykkz000
-- DateCreated: 9/25/2026
--------------------------------------------------------------
-- Attach the Stalin suzerain combat bonus modifier to every city-state trait
INSERT OR IGNORE INTO TraitModifiers (TraitType, ModifierId)
SELECT DISTINCT lt.TraitType, 'YKKZ000_STALIN_SUZERAIN_ATTACH'
FROM LeaderTraits lt
JOIN CivilizationLeaders cl ON cl.LeaderType = lt.LeaderType
JOIN Civilizations c ON c.CivilizationType = cl.CivilizationType
WHERE c.StartingCivilizationLevelType = 'CIVILIZATION_LEVEL_CITY_STATE';

INSERT OR IGNORE INTO TraitModifiers (TraitType, ModifierId)
SELECT DISTINCT ct.TraitType, 'YKKZ000_STALIN_SUZERAIN_ATTACH'
FROM CivilizationTraits ct
JOIN Civilizations c ON c.CivilizationType = ct.CivilizationType
WHERE c.StartingCivilizationLevelType = 'CIVILIZATION_LEVEL_CITY_STATE';
