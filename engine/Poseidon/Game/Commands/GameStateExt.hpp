#pragma once

#include <Evaluator/express.hpp>
#include <Poseidon/World/Scene/Object.hpp>
#include <Poseidon/Network/NetworkObject.hpp>

namespace Poseidon
{
} // namespace Poseidon


const GameType GameObject(0x100);
const GameType GameVector(0x200);
const GameType GameTrans(0x400);
const GameType GameOrient(0x800);
const GameType GameSide(0x1000);
const GameType GameGroup(0x2000);
const GameType GameFile(0x4000);
const GameType GameDatabase(0x8000);

typedef TargetSide GameSideType;

typedef OLink<Object> GameObjectType;
typedef OLink<AIGroup> GameGroupType;
typedef Vector3 GameVectorType;
typedef Matrix4 GameTransType;
typedef Matrix3 GameOrientType;

class GameFileType
{
private:
	int _index;

public:
	bool readOnly;

	GameFileType() {_index = -1; readOnly = false;}
	GameFileType(const GameFileType &src);
	~GameFileType();

	void operator = (const GameFileType &src);

	int GetIndex() const {return _index;}
	void SetIndex(int index);
};

class GameDatabaseType
{
private:
    int _index;

public:
    GameDatabaseType() {_index = -1;}
    GameDatabaseType(const GameDatabaseType &src);
    ~GameDatabaseType();

    void operator=(const GameDatabaseType &src);

    int GetIndex() const { return _index; }
    void SetIndex(int index);
};

class GameDataObject: public GameData
{
	typedef GameData base;

	GameObjectType _value;

	public:
	GameDataObject();
	GameDataObject( GameObjectType value );
	~GameDataObject() override{}

	GameType GetType() const override {return GameObject;}
	GameObjectType GetObject() const;

	RString GetText() const override;
	bool IsEqualTo(const GameData *data) const override;
	const char *GetTypeName() const override {return "object";}
	GameData *Clone() const override;

	LSError Serialize(ParamArchive &ar) override;

	USE_FAST_ALLOCATOR
};

class GameDataGroup: public GameData
{
	typedef GameData base;

	GameGroupType _value;

	public:
	GameDataGroup();
	GameDataGroup( GameGroupType value );
	~GameDataGroup() override{}

	GameType GetType() const override {return GameGroup;}
	GameGroupType GetGroup() const;

	RString GetText() const override;
	bool IsEqualTo(const GameData *data) const override;
	const char *GetTypeName() const override {return "group";}
	GameData *Clone() const override;

	LSError Serialize(ParamArchive &ar) override;

	USE_FAST_ALLOCATOR
};

class GameDataSide: public GameData
{
	typedef GameData base;

	GameSideType _value;

	public:
	GameDataSide():_value(TSideUnknown){}
	GameDataSide( GameSideType value ):_value(value){}
	~GameDataSide() override{}

	GameType GetType() const override {return GameSide;}
	GameSideType GetSide() const {return _value;}

	RString GetText() const override;
	bool IsEqualTo(const GameData *data) const override;
	const char *GetTypeName() const override {return "side";}
	GameData *Clone() const override {return new GameDataSide(*this);}

	LSError Serialize(ParamArchive &ar) override;

	USE_FAST_ALLOCATOR
};

class GameDataFile: public GameData
{
	typedef GameData base;

	GameFileType _value;

	public:
	GameDataFile(){}
	GameDataFile( GameFileType value ):_value(value){}
	~GameDataFile() override{}

	GameType GetType() const override {return GameFile;}
	GameFileType GetFile() const {return _value;}

	RString GetText() const override;
	bool IsEqualTo(const GameData *data) const override;
	const char *GetTypeName() const override {return "file";}
	GameData *Clone() const override {return new GameDataFile(*this);}

	LSError Serialize(ParamArchive &ar) override;

	USE_FAST_ALLOCATOR
};

class GameDataDatabase : public GameData
{
    typedef GameData base;

    GameDatabaseType _value;

public:
    GameDataDatabase(){}
    GameDataDatabase( GameDatabaseType value ):_value(value){}
    ~GameDataDatabase() override{}

    GameType GetType() const override {return GameDatabase;}
    GameDatabaseType GetDatabase() const {return _value;}

    RString GetText() const override;
    bool IsEqualTo(const GameData *data) const override;
    const char *GetTypeName() const override {return "database";}
    GameData* Clone() const override {return new GameDataDatabase(*this);}

    LSError Serialize(ParamArchive &ar) override;

    USE_FAST_ALLOCATOR
};

GameData* CreateGameDataDatabase();

inline TargetSide GetSide( GameValuePar oper )
{
	if( oper.GetType()!=GameSide ) return TSideUnknown;
	return static_cast<GameDataSide *>(oper.GetData())->GetSide();
}

typedef bool (AICenter::*IsSide)( TargetSide side) const;

namespace Poseidon { class EntityType; }

class GameValueExt: public GameValue
{
	public:

	GameValueExt( AIGroup *value );
	GameValueExt( Object *value );
	GameValueExt( GameSideType value ) {_data=new GameDataSide(value);}
	GameValueExt( GameVectorType value );
	GameValueExt( GameTransType value );
	GameValueExt( GameOrientType value );
	GameValueExt( GameFileType value ) {_data=new GameDataFile(value);}
	GameValueExt( GameDatabaseType value ) {_data=new GameDataDatabase(value);}
};

inline GameValue CreateGameSide( TargetSide side ) {return GameValueExt(side);}
