# In-Memory Database

The game database is an independent, in-memory key-value store exposed to scripts. It does not use the native `ParamFile` configuration commands and does not read or write config-style `.cfg` files.

Database data is held in memory until explicitly saved. Persistence uses the database's binary `PDB1` format.

## Creating and loading databases

```sqf
_db = dbNew;
_loaded = dbLoad "players.db";
```

`dbNew` returns a new empty database handle.

`dbLoad` returns a database handle when the file exists and is valid. Missing or corrupt files produce an invalid database handle and an error in the game log.

Database files are stored under the current profile's configuration directory:

```text
%APPDATA%/CWR/Users/<profile>/Config/<filename>
```

Pass only a filename. Path separators are rejected.

## Saving

```sqf
success = _db dbSave "players.db";
```

Returns `true` when the database is successfully written and `false` on failure.

Saving writes a temporary file and then replaces the destination. Existing files are overwritten. The file is binary and should not be edited as a text config file.

## General keys and values

Keys may use dotted paths. Intermediate components are hashes and are created by `dbSet` as needed.

```sqf
_db dbSet ["players.UID123.name", "JSchmidt"];
_db dbSet ["players.UID123.score", 9999];
_db dbSet ["players.UID123.active", true];
```

`dbSet` returns `true` when the value is stored and `false` for invalid input. It replaces an existing value at the key, including changing its type.

```sqf
name = _db dbGet ["players.UID123.name", "unknown"];
score = _db dbGet ["players.UID123.score", 0];
```

`dbGet` returns the stored value or the supplied default when the key does not exist.

```sqf
exists = _db dbExists "players.UID123.score";
_db dbDelete "players.UID123.score";
keys = _db dbKeys "players.UID123";
```

`dbExists` returns a boolean. `dbDelete` returns `true` when a key is removed and `false` when it does not exist or the input is invalid. `dbKeys` returns the child names of a hash as an array. Hash key ordering is not guaranteed.

## Lists

Lists are zero-indexed.

### Append one or many values

```sqf
newSize = _db dbListPush ["inventory", "rifle"];
newSize = _db dbListPushMany ["inventory", ["medkit", "radio", "map"]];
```

`dbListPush` appends one value and returns the new list size.

`dbListPushMany` appends all values in order and returns the new list size. The operation fails with `-1` without changing the list if the arguments are invalid.

### Read values

```sqf
item = _db dbListGet ["inventory", 1, "missing"];
allItems = _db dbGet ["inventory", []];
size = _db dbListSize "inventory";
```

`dbListGet` returns the value at the index, or the fallback value when the list or index is invalid.

`dbGet` returns the entire list. `dbListSize` returns the number of items, or `0` when the key is not a list.

### Update and remove values

```sqf
ok = _db dbListSet ["inventory", 1, "bandage"];
ok = _db dbListRemove ["inventory", 0];
```

Both return `true` when the indexed operation succeeds and `false` when the list or index is invalid.

```sqf
last = _db dbListPop ["inventory"];
item = _db dbListPop ["inventory", 1, "missing"];
```

`dbListPop` removes and returns the last item when no index is supplied. With an index, it removes and returns that item. The optional fallback is returned for an invalid list or index.

## Hashes

Hashes store field/value pairs. They are useful for player records and named attributes.

### Set one or many fields

```sqf
ok = _db dbHashSet ["players.UID123.stats", "score", 9999];

ok = _db dbHashSetMany [
    "players.UID123.stats",
    [
        ["rank", "captain"],
        ["missions", 12],
        ["active", true]
    ]
];
```

`dbHashSet` sets one field and returns `true` or `false`.

`dbHashSetMany` sets all supplied pairs and returns `true` or `false`. It validates the complete input before changing the hash. Existing fields are replaced.

### Read fields and entries

```sqf
score = _db dbHashGet ["players.UID123.stats", "score", 0];
fields = _db dbHashKeys "players.UID123.stats";
allFields = _db dbGet ["players.UID123.stats", []];
```

`dbHashGet` returns the field value or the fallback value when the field is missing.

`dbHashKeys` returns the field names. `dbGet` returns hash entries as key/value pairs:

```text
[["score", 9999], ["rank", "captain"]]
```

Hash ordering is not guaranteed.

### Test and delete fields

```sqf
exists = _db dbHashExists ["players.UID123.stats", "score"];
removed = _db dbHashDelete ["players.UID123.stats", "score"];
```

`dbHashExists` returns whether the field exists. `dbHashDelete` returns `true` when a field was removed and `false` otherwise.

## Complete example

```sqf
_db = dbNew;

_db dbSet ["players.UID123.name", "JSchmidt"];
_db dbSet ["players.UID123.score", 9999];
_db dbListPushMany ["players.UID123.inventory", ["rifle", "medkit", "radio"]];
_db dbHashSetMany [
    "players.UID123.stats",
    [["rank", "captain"], ["missions", 12]]
];

if (_db dbSave "players.db") then {
    logInfo "Player database saved";
};

loaded = dbLoad "players.db";
loadedScore = loaded dbGet ["players.UID123.score", 0];
loadedInventory = loaded dbGet ["players.UID123.inventory", []];
loadedStats = loaded dbGet ["players.UID123.stats", []];
```

## Snapshot and concurrency behavior

`dbLoad` creates an independent in-memory snapshot. Later changes made to the file by another database handle or process are not visible to an existing handle.

If multiple handles save the same filename, the last successful save wins. The database is intended for one authoritative game/server process, not as a multi-process database server.

## Supported value types

Values may be:

- strings;
- numbers;
- booleans;
- lists containing supported values;
- hashes containing supported values.

Game objects and other engine-specific handles are not persistable database values.

## Command summary

| Command | Return value |
|---|---|
| `dbNew` | database handle |
| `dbLoad filename` | database handle or invalid handle |
| `dbSave filename` | boolean |
| `dbSet [key, value]` | boolean |
| `dbGet [key, default]` | value or default |
| `dbDelete key` | boolean |
| `dbExists key` | boolean |
| `dbKeys key` | array of child keys |
| `dbListPush [key, value]` | new size or `-1` |
| `dbListPushMany [key, values]` | new size or `-1` |
| `dbListPop [key, index, default]` | removed value or default |
| `dbListGet [key, index, default]` | value or default |
| `dbListSet [key, index, value]` | boolean |
| `dbListRemove [key, index]` | boolean |
| `dbListSize key` | size |
| `dbHashSet [key, field, value]` | boolean |
| `dbHashSetMany [key, pairs]` | boolean |
| `dbHashGet [key, field, default]` | value or default |
| `dbHashDelete [key, field]` | boolean |
| `dbHashExists [key, field]` | boolean |
| `dbHashKeys key` | array of field names |
