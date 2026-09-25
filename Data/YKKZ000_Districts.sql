-- YKKZ000_Districts
-- Author: ykkz000
-- DateCreated: 9/25/2026
--------------------------------------------------------------
-- (a) Provide standard adjacency bonuses to adjacent districts
INSERT OR IGNORE INTO Adjacency_YieldChanges (ID, Description, YieldType, YieldChange, TilesRequired, AdjacentDistrict) VALUES
  ('YKKZ000_KOLKHOZ_PROVIDE_SCIENCE',   'LOC_DISTRICT_YKKZ000_KOLKHOZ_NAME', 'YIELD_SCIENCE',   1, 1, 'DISTRICT_YKKZ000_KOLKHOZ'),
  ('YKKZ000_KOLKHOZ_PROVIDE_CULTURE',   'LOC_DISTRICT_YKKZ000_KOLKHOZ_NAME', 'YIELD_CULTURE',   1, 1, 'DISTRICT_YKKZ000_KOLKHOZ'),
  ('YKKZ000_KOLKHOZ_PROVIDE_FAITH',     'LOC_DISTRICT_YKKZ000_KOLKHOZ_NAME', 'YIELD_FAITH',     1, 1, 'DISTRICT_YKKZ000_KOLKHOZ'),
  ('YKKZ000_KOLKHOZ_PROVIDE_GOLD',      'LOC_DISTRICT_YKKZ000_KOLKHOZ_NAME', 'YIELD_GOLD',      1, 1, 'DISTRICT_YKKZ000_KOLKHOZ'),
  ('YKKZ000_KOLKHOZ_PROVIDE_PRODUCTION','LOC_DISTRICT_YKKZ000_KOLKHOZ_NAME', 'YIELD_PRODUCTION',1, 1, 'DISTRICT_YKKZ000_KOLKHOZ');

INSERT OR IGNORE INTO District_Adjacencies (DistrictType, YieldChangeId)
SELECT DistrictType, 'YKKZ000_KOLKHOZ_PROVIDE_SCIENCE' FROM Districts WHERE DistrictType = 'DISTRICT_CAMPUS';
INSERT OR IGNORE INTO District_Adjacencies (DistrictType, YieldChangeId)
SELECT DistrictType, 'YKKZ000_KOLKHOZ_PROVIDE_CULTURE' FROM Districts WHERE DistrictType IN ('DISTRICT_THEATER','DISTRICT_ACROPOLIS');
INSERT OR IGNORE INTO District_Adjacencies (DistrictType, YieldChangeId)
SELECT DistrictType, 'YKKZ000_KOLKHOZ_PROVIDE_FAITH' FROM Districts WHERE DistrictType IN ('DISTRICT_HOLY_SITE','DISTRICT_LAVRA');
INSERT OR IGNORE INTO District_Adjacencies (DistrictType, YieldChangeId)
SELECT DistrictType, 'YKKZ000_KOLKHOZ_PROVIDE_GOLD' FROM Districts WHERE DistrictType IN ('DISTRICT_COMMERCIAL_HUB','DISTRICT_HARBOR','DISTRICT_ROYAL_NAVY_DOCKYARD');
INSERT OR IGNORE INTO District_Adjacencies (DistrictType, YieldChangeId)
SELECT DistrictType, 'YKKZ000_KOLKHOZ_PROVIDE_PRODUCTION' FROM Districts WHERE DistrictType IN ('DISTRICT_INDUSTRIAL_ZONE','DISTRICT_HANSA');

-- (b) Industrial Zone buildings provide Food equal to their Production
INSERT OR IGNORE INTO Modifiers (ModifierId, ModifierType, SubjectRequirementSetId)
SELECT 'YKKZ000_KOLKHOZ_FOOD_' || b.BuildingType,
       'MODIFIER_PLAYER_CITIES_ADJUST_BUILDING_YIELD_CHANGE',
       'REQUIRESETS_YKKZ000_CITY_HAS_KOLKHOZ'
FROM Buildings b
JOIN Building_YieldChanges y ON y.BuildingType = b.BuildingType AND y.YieldType = 'YIELD_PRODUCTION'
WHERE b.PrereqDistrict = 'DISTRICT_INDUSTRIAL_ZONE';

INSERT OR IGNORE INTO ModifierArguments (ModifierId, Name, Value)
SELECT 'YKKZ000_KOLKHOZ_FOOD_' || b.BuildingType, 'BuildingType', b.BuildingType
FROM Buildings b JOIN Building_YieldChanges y ON y.BuildingType = b.BuildingType AND y.YieldType = 'YIELD_PRODUCTION'
WHERE b.PrereqDistrict = 'DISTRICT_INDUSTRIAL_ZONE';

INSERT OR IGNORE INTO ModifierArguments (ModifierId, Name, Value)
SELECT 'YKKZ000_KOLKHOZ_FOOD_' || b.BuildingType, 'YieldType', 'YIELD_FOOD'
FROM Buildings b JOIN Building_YieldChanges y ON y.BuildingType = b.BuildingType AND y.YieldType = 'YIELD_PRODUCTION'
WHERE b.PrereqDistrict = 'DISTRICT_INDUSTRIAL_ZONE';

INSERT OR IGNORE INTO ModifierArguments (ModifierId, Name, Value)
SELECT 'YKKZ000_KOLKHOZ_FOOD_' || b.BuildingType, 'Amount', CAST(y.YieldChange AS TEXT)
FROM Buildings b JOIN Building_YieldChanges y ON y.BuildingType = b.BuildingType AND y.YieldType = 'YIELD_PRODUCTION'
WHERE b.PrereqDistrict = 'DISTRICT_INDUSTRIAL_ZONE';

INSERT OR IGNORE INTO TraitModifiers (TraitType, ModifierId)
SELECT 'TRAIT_CIVILIZATION_YKKZ000_KOLKHOZ', 'YKKZ000_KOLKHOZ_FOOD_' || b.BuildingType
FROM Buildings b JOIN Building_YieldChanges y ON y.BuildingType = b.BuildingType AND y.YieldType = 'YIELD_PRODUCTION'
WHERE b.PrereqDistrict = 'DISTRICT_INDUSTRIAL_ZONE';

-- (c) Industrial Zone buildings provide 1 Housing
INSERT OR IGNORE INTO Modifiers (ModifierId, ModifierType, SubjectRequirementSetId)
SELECT 'YKKZ000_KOLKHOZ_HOUSING_' || b.BuildingType,
       'MODIFIER_PLAYER_CITIES_ADJUST_BUILDING_HOUSING',
       'REQUIRESETS_YKKZ000_CITY_HAS_KOLKHOZ'
FROM Buildings b WHERE b.PrereqDistrict = 'DISTRICT_INDUSTRIAL_ZONE';

INSERT OR IGNORE INTO ModifierArguments (ModifierId, Name, Value)
SELECT 'YKKZ000_KOLKHOZ_HOUSING_' || b.BuildingType, 'Amount', '1'
FROM Buildings b WHERE b.PrereqDistrict = 'DISTRICT_INDUSTRIAL_ZONE';

INSERT OR IGNORE INTO BuildingModifiers (BuildingType, ModifierId)
SELECT b.BuildingType, 'YKKZ000_KOLKHOZ_HOUSING_' || b.BuildingType
FROM Buildings b WHERE b.PrereqDistrict = 'DISTRICT_INDUSTRIAL_ZONE';
