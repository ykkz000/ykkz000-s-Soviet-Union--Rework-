-- Replace the GameCore DLL with the YKKZ000 loader for Gathering Storm (Expansion2) only
UPDATE GameCores
SET PackageId = '65607241-07c8-4ffc-98cc-a465f65e1a64',
    DllPrefix = 'GameCore_YKKZ000_Loader_XP2'
WHERE GameCore = 'Expansion2';
