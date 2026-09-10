#pragma once

#include <Poseidon/Foundation/Strings/RString.hpp>
#include <Poseidon/Foundation/Time/Time.hpp>
#include <Poseidon/Foundation/Containers/Array.hpp>
#include <Poseidon/Foundation/Containers/StaticArray.hpp>
#include <Poseidon/Foundation/Types/LLinks.hpp>
#include <Poseidon/Foundation/Enums/EnumNames.hpp>
#include <Poseidon/IO/Streams/QStream.hpp>
#include <Poseidon/Graphics/Rendering/Colors.hpp>
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/IO/Serialization/SerializeClass.hpp>
#include <Poseidon/Foundation/Types/RemoveLinks.hpp>

using Poseidon::LSError;
using Poseidon::LSOK;
using Poseidon::LSNoEntry;

// macros
#define ON_ERROR(err, ctx) {OnError(err, ctx); return err;}
#define CANCEL_ERROR {ParamArchive::CancelError(); return LSOK;}

void TraceError(const char *command);

#undef PARAM_CHECK
#define PARAM_CHECK(command) \
	{LSError err = command; if (err != LSOK) {TraceError(#command); return err;}}
#define SERIAL(name) \
	PARAM_CHECK(ar.Serialize(#name, _##name, 1))
#define SERIAL_DEF(name, value) \
	PARAM_CHECK(ar.Serialize(#name, _##name, 1, value))
#define SERIAL_ENUM(name, value) \
	PARAM_CHECK(ar.SerializeEnum(#name, _##name, 1, value))
#define SERIAL_BASE \
	PARAM_CHECK(base::Serialize(ar))
#define SerializeBitField(type,ar,name,var,version,def) \
	{ \
		type t=var; \
		PARAM_CHECK(ar.Serialize(name, t, version, def)); \
		var=t; \
	}
#define SerializeBitBool(ar,name,var,version,def) \
	SerializeBitField(bool, ar, name, var, version, def)
#define SERIAL_BITFIELD(type, name, value) \
	SerializeBitField(type, ar, #name, _##name, 1, value)
#define SERIAL_BITBOOL(name, value) \
	SerializeBitField(bool, ar, #name, _##name, 1, value)

#define PARS_4_TO_3_FUNC(func)												\
{																											\
	if (_version < minVersion)													\
	{																										\
		if (!_saving) value = defValue;										\
		return LSOK;																			\
	}																										\
	if (_saving && value == defValue) return LSOK;			\
	LSError err = func(name, value, minVersion);	    	\
	if (err == LSNoEntry)																\
	{value = defValue; CANCEL_ERROR;}										\
	return err;																					\
}

#define PARS_4_TO_3 PARS_4_TO_3_FUNC(Serialize)

#define PARS_4_TO_3_NODEFAULTS												\
{																											\
	if (_version < minVersion)													\
	{																										\
		if (!_saving) value = defValue;										\
		return LSOK;																			\
	}																										\
	return Serialize(name, value, minVersion);					\
}

namespace Poseidon::Foundation { struct EnumName; }
namespace Poseidon { class ParamEntry; }
using Poseidon::ParamEntry;
#include <Poseidon/IO/ParamFile/ParamFile.hpp>

class ParamArchive
{
public:
	enum Pass
	{
		PassUndefined,
		PassFirst,
		PassSecond
	};
protected:
	InitPtr<ParamEntry> _entry;
	int _version;
	Pass _pass;
	void *_params;
	bool _saving;

	static RString _errorContext;

public:
	ParamArchive();
	virtual ~ParamArchive() {};

	bool IsSaving() const {return _saving;}
	bool IsLoading() const {return !_saving;}
	bool IsValid() const {return _entry != nullptr;}
	int GetArVersion() const {return _version;}
	LSError CheckMaxVersion(int maxVersion);
	Pass GetPass() const {return _pass;}
	void FirstPass() {_pass = PassFirst;}
	void SecondPass() {_pass = PassSecond;}
	void SetParams(void *params) {_params = params;}
	void *GetParams() {return _params;}

	bool IsSubclass(const RStringB &name);
	bool OpenSubclass(const RStringB &name, ParamArchive &ar, bool guaranteedUnique=false);
	void CloseSubclass(ParamArchive &ar);

	void OnError(LSError err, RString context);	
	static void CancelError();	
	static RString GetErrorContext() {return _errorContext;}
	static const char *GetErrorName(LSError err);
	LSError Serialize(const RStringB &name, bool &value, int minVersion);
	LSError Serialize(const RStringB &name, bool &value, int minVersion, bool defValue);
	LSError Serialize(const RStringB &name, int &value, int minVersion);
	LSError Serialize(const RStringB &name, int &value, int minVersion, int defValue);
	LSError Serialize(const RStringB &name, unsigned char &value, int minVersion);
	LSError Serialize(const RStringB &name, unsigned char &value, int minVersion, unsigned char defValue);
	LSError Serialize(const RStringB &name, float &value, int minVersion);
	LSError Serialize(const RStringB &name, float &value, int minVersion, float defValue);
	LSError Serialize(const RStringB &name, Poseidon::Foundation::Time &value, int minVersion);
	LSError Serialize(const RStringB &name, Poseidon::Foundation::Time &value, int minVersion, Poseidon::Foundation::Time defValue);
	LSError Serialize(const RStringB &name, Poseidon::Foundation::TimeSec &value, int minVersion);
	LSError Serialize(const RStringB &name, Poseidon::Foundation::TimeSec &value, int minVersion, Poseidon::Foundation::TimeSec defValue);
	LSError Serialize(const RStringB &name, RString &value, int minVersion);
	LSError Serialize(const RStringB &name, RString &value, int minVersion, RString defValue);
	LSError Serialize(const RStringB &name, Vector3 &value, int minVersion);
	LSError Serialize(const RStringB &name, Vector3 &value, int minVersion, Vector3Par defValue);	// use Vector3Par instead Vector3
	LSError Serialize(const RStringB &name, Matrix4 &value, int minVersion);
	LSError Serialize(const RStringB &name, Matrix4 &value, int minVersion, Matrix4 &defValue);	// use Matrix4 & instead Matrix4
	LSError Serialize(const RStringB &name, Color &value, int minVersion);
	LSError Serialize(const RStringB &name, Color &value, int minVersion, Color defValue);
	LSError Serialize(const RStringB &name, SerializeClass &value, int minVersion);

	LSError SerializeEnumValue
	(
		const RStringB &name, int &value, int minVersion, const Poseidon::Foundation::EnumName *names
	);

	template <class EnumType>
	LSError SerializeEnum(const RStringB &name, EnumType &value, int minVersion)
	{
		int iVal = value;
		LSError ret = SerializeEnumValue(name,iVal,minVersion,Poseidon::Foundation::GetEnumNames(value));
		value = EnumType(iVal);
		return ret;
	}

	template <class EnumType>
	LSError SerializeEnum(const RStringB &name, EnumType &value, int minVersion, EnumType defValue)
	PARS_4_TO_3_FUNC(SerializeEnum)

	template <class Type>
	LSError Serialize(const RStringB &name, SRef<Type> &value, int minVersion)
	{
		if (_version < minVersion) return LSOK;
		if (_saving || _pass == PassSecond)
		{
			if (!value) return LSOK;
			ParamArchive arRef;
			if (!OpenSubclass(name, arRef)) ON_ERROR(LSNoEntry, name);
			PARAM_CHECK(value->Serialize(arRef))
			CloseSubclass(arRef);
		}
		else
		{
			PoseidonAssert(_pass == PassFirst);
			ParamArchive arRef;
			if (!OpenSubclass(name, arRef))
			{
				value = nullptr;
				return LSOK;
			}
			value = Type::CreateObject(arRef);
			if (value) PARAM_CHECK(value->Serialize(arRef))
			CloseSubclass(arRef);
		}
		return LSOK;
	}
	template <class Type>
	LSError Serialize(const RStringB &name, Ref<Type> &value, int minVersion)
	{
		if (_version < minVersion) return LSOK;
		if (_saving || _pass == PassSecond)
		{
			if (!value) return LSOK;
			ParamArchive arRef;
			if (!OpenSubclass(name, arRef)) ON_ERROR(LSNoEntry, name);
			PARAM_CHECK(value->Serialize(arRef))
			CloseSubclass(arRef);
		}
		else
		{
			PoseidonAssert(_pass == PassFirst);
			ParamArchive arRef;
			if (!OpenSubclass(name, arRef))
			{
				value = nullptr;
				return LSOK;
			}
			value = Type::CreateObject(arRef);
			if (value) PARAM_CHECK(value->Serialize(arRef))
			CloseSubclass(arRef);
		}
		return LSOK;
	}
	template <class Type>
	LSError SerializeRef(const RStringB &name, Ref<Type> &value, int minVersion)
	{
		if (_version < minVersion) return LSOK;
		if (_saving)
		{
			if (!value) return LSOK;
			ParamArchive arRef;
			if (!OpenSubclass(name, arRef)) ON_ERROR(LSNoEntry, name);
			PARAM_CHECK(value->SaveRef(arRef))
			CloseSubclass(arRef);
		}
		else
		{
			if (_pass != PassSecond) return LSOK;
			ParamArchive arRef;
			if (!OpenSubclass(name, arRef))
			{
				value = nullptr;
				return LSOK;
			}
			arRef._pass = PassFirst;	// allow LoadRef
			value = dynamic_cast<Type *>(Type::LoadRef(arRef));
			CloseSubclass(arRef);
		}
		return LSOK;
	}
	template <class Type>
	LSError SerializeRef(const RStringB &name, Link<Type> &value, int minVersion)
	{
		if (_version < minVersion) return LSOK;
		if (_saving)
		{
			if (!value) return LSOK;
			ParamArchive arRef;
			if (!OpenSubclass(name, arRef)) ON_ERROR(LSNoEntry, name);
			PARAM_CHECK(value->SaveRef(arRef))
			CloseSubclass(arRef);
		}
		else
		{
			if (_pass != PassSecond) return LSOK;
			ParamArchive arRef;
			if (!OpenSubclass(name, arRef))
			{
				value = nullptr;
				return LSOK;
			}
			arRef._pass = PassFirst;	// allow LoadRef
			value = dynamic_cast<Type *>(Type::LoadRef(arRef));
			CloseSubclass(arRef);
		}
		return LSOK;
	}
	template <class Type>
	LSError SerializeRef(const RStringB &name, LLink<Type> &value, int minVersion)
	{
		if (_version < minVersion) return LSOK;
		if (_saving)
		{
			if (!value) return LSOK;
			ParamArchive arRef;
			if (!OpenSubclass(name, arRef)) ON_ERROR(LSNoEntry, name);
			PARAM_CHECK(value->SaveRef(arRef))
			CloseSubclass(arRef);
		}
		else
		{
			if (_pass != PassSecond) return LSOK;
			ParamArchive arRef;
			if (!OpenSubclass(name, arRef))
			{
				value = nullptr;
				return LSOK;
			}
			arRef._pass = PassFirst;	// allow LoadRef
			value = dynamic_cast<Type *>(Type::LoadRef(arRef));
			CloseSubclass(arRef);
		}
		return LSOK;
	}
	template <class Type>
	LSError SerializeArray(const RStringB &name, AutoArray<Type> &value, int minVersion)
	{
		if (_version < minVersion) return LSOK;
		if (_saving)
		{
			if (value.Size()==0) return LSOK; // default value - empty array
			ParamEntry *array = _entry->AddArray(name);
			array->ReserveArrayElements(value.Size());
			for (int i=0; i<value.Size(); i++)
			{
				array->AddValue(value[i]);
			}
		}
		else
		{
			if (_pass != PassFirst) return LSOK;
			ParamEntry *array = _entry->FindEntry(name);
			if (!array)
			{
				value.Resize(0);
				return LSOK;
			}
			int n = array->GetSize();
			value.Realloc(n);
			value.Resize(n);
			for (int i=0; i<n; i++)
				value[i] = Type((*array)[i]);
		}
		return LSOK;
	}
	
	LSError SerializeArray(const RStringB &name, AutoArray<Vector3> &value, int minVersion);

	template <class Type>
	LSError SerializeArrayItemDef(const char *name, ParamArchive &arCls, Type &valueI)
	{
		ParamArchive arItem;
		if (arCls.IsSaving() && valueI.IsDefaultValue(arCls))
		{
			return LSOK;
		}
		if (!arCls.OpenSubclass(name, arItem, true))
		{
			if (arCls.IsLoading())
			{
				// use default values
				valueI.LoadDefaultValues(arCls);
			}
			else
			{
				ON_ERROR(LSNoEntry, arCls._entry->GetContext() + RString(".") + RString(name));
			}
		}
		else
		{
			PARAM_CHECK(valueI.Serialize(arItem))
			arCls.CloseSubclass(arItem);
		}
		return LSOK;
	}
	template <class Type>
	LSError SerializeArrayItem(const char *name, ParamArchive &arCls, Type &valueI)
	{
		ParamArchive arItem;
		if (!arCls.OpenSubclass(name, arItem, true))
		{
			ON_ERROR(LSNoEntry, arCls._entry->GetContext() + RString(".") + RString(name));
		}
		else
		{
			PARAM_CHECK(valueI.Serialize(arItem))
			arCls.CloseSubclass(arItem);
		}
		return LSOK;
	}
	template <class Type, class Allocator>
	LSError Serialize(const RStringB &name, AutoArray<Type, Allocator> &value, int minVersion)
	{
		if (_version < minVersion) return LSOK;
	// call Serialize for both passes
		ParamArchive arCls;
		int n;
		if (_saving)
		{
			n = value.Size();
			if( n==0 ) return LSOK;

			if (!OpenSubclass(name, arCls)) ON_ERROR(LSNoEntry, name);

			PARAM_CHECK(arCls.Serialize("items", n, minVersion))
		}
		else if (_pass == PassSecond)
		{
			n = value.Size();
			if (!OpenSubclass(name, arCls))
			{
				if( n==0 ) return LSOK;
				ON_ERROR(LSNoEntry, name);
			}
		}
		else
		{
			if (!OpenSubclass(name, arCls))
			{
				value.Clear();
				return LSOK;
			}
			PARAM_CHECK(arCls.Serialize("items", n, minVersion))
			value.Resize(n);
		}
		for (int i=0; i<n; i++)
		{
			char buffer[256];
			snprintf(buffer, sizeof(buffer), "Item%d", i);
			SerializeArrayItem(buffer,arCls,value[i]);
		}
		CloseSubclass(arCls);
		return LSOK;
	}
	template <class Type, class Allocator>
	LSError SerializeDef(const RStringB &name, AutoArray<Type, Allocator> &value, int minVersion)
	{
		if (_version < minVersion) return LSOK;
	// call Serialize for both passes
		ParamArchive arCls;
		int n;
		if (_saving)
		{
			n = value.Size();
			if( n==0 ) return LSOK;

			if (!OpenSubclass(name, arCls)) ON_ERROR(LSNoEntry, name);

			PARAM_CHECK(arCls.Serialize("items", n, minVersion))
		}
		else if (_pass == PassSecond)
		{
			n = value.Size();
			if (!OpenSubclass(name, arCls))
			{
				if( n==0 ) return LSOK;
				ON_ERROR(LSNoEntry, name);
			}
		}
		else
		{
			if (!OpenSubclass(name, arCls))
			{
				value.Clear();
				return LSOK;
			}
			PARAM_CHECK(arCls.Serialize("items", n, minVersion))
			value.Resize(n);
		}
		for (int i=0; i<n; i++)
		{
			char buffer[256];
			snprintf(buffer, sizeof(buffer), "Item%d", i);
			SerializeArrayItemDef(buffer,arCls,value[i]);
		}
		CloseSubclass(arCls);
		return LSOK;
	}
	template<class Type, class Container>
	LSError Serialize(const RStringB &name, MapStringToClass<Type, Container> &value, int minVersion)
	{
		if (_version < minVersion) return LSOK;
		// call Serialize for both passes
		ParamArchive arCls;
		if (!OpenSubclass(name, arCls)) ON_ERROR(LSNoEntry, name);
		if (_saving)
		{
			int ii = 0, nn = value.NItems();
			PARAM_CHECK(arCls.Serialize("items", nn, minVersion))
			if (nn > 0)
			{
				// !!! avoid GetTable when NItems == 0
				int n = value.NTables();
				for (int i=0; i<n; i++)
				{
					Container &container = value.GetTable(i);
					int m = container.Size();
					for (int j=0; j<m; j++)
					{
						Type &item = container[j];
						char buffer[256];
						snprintf(buffer, sizeof(buffer), "Item%d", ii++);
						ParamArchive arItem;
						if (!arCls.OpenSubclass(buffer, arItem,true)) ON_ERROR(LSNoEntry, name + RString(".") + RString(buffer));
						PARAM_CHECK(item.Serialize(arItem))
						arCls.CloseSubclass(arItem);
					}
				}
			}
		}
		else if (_pass == PassSecond)
		{
			int ii = 0, nn = value.NItems();
			if (nn > 0)
			{
				// !!! avoid GetTable when NItems == 0
				int n = value.NTables();
				for (int i=0; i<n; i++)
				{
					Container &container = value.GetTable(i);
					int m = container.Size();
					for (int j=0; j<m; j++)
					{
						Type &item = container[j];
						char buffer[256];
						snprintf(buffer, sizeof(buffer), "Item%d", ii++);
						ParamArchive arItem;
						if (!arCls.OpenSubclass(buffer, arItem, true)) ON_ERROR(LSNoEntry, name + RString(".") + RString(buffer));
						PARAM_CHECK(item.Serialize(arItem))
						arCls.CloseSubclass(arItem);
					}
				}
			}
		}
		else
		{
			int nn;
			PARAM_CHECK(arCls.Serialize("items", nn, minVersion))
			for (int ii=0; ii<nn; ii++)
			{
				char buffer[256];
				snprintf(buffer, sizeof(buffer), "Item%d", ii);
				ParamArchive arItem;
				if (!arCls.OpenSubclass(buffer, arItem, true)) ON_ERROR(LSNoEntry, name + RString(".") + RString(buffer));
				Type item;
				PARAM_CHECK(item.Serialize(arItem))
				value.Add(item);
				arCls.CloseSubclass(arItem);
			}
		}
		CloseSubclass(arCls);
		return LSOK;
	}
	template <class Type>
	LSError Serialize(const RStringB &name, RefArray<Type> &value, int minVersion)
	{
		if (_version < minVersion) return LSOK;
	// call Serialize for both passes
		ParamArchive arCls;
		int n;
		if (_saving)
		{
			n = value.Size();
			if( n==0 ) return LSOK;

			if (!OpenSubclass(name, arCls)) ON_ERROR(LSNoEntry, name);

			PARAM_CHECK(arCls.Serialize("items", n, minVersion))
		}
		else if (_pass == PassSecond)
		{
			n = value.Size();
			if (!OpenSubclass(name, arCls))
			{
				if( n==0 ) return LSOK;
				ON_ERROR(LSNoEntry, name);
			}
		}
		else
		{
			if (!OpenSubclass(name, arCls))
			{
				value.Clear();
				return LSOK;
			}
			PARAM_CHECK(arCls.Serialize("items", n, minVersion))
			value.Resize(n);
		}
		for (int i=0; i<n; i++)
		{
			char buffer[256];
			snprintf(buffer, sizeof(buffer), "Item%d", i);
			PARAM_CHECK(arCls.Serialize(buffer, value[i], minVersion));
		}
		CloseSubclass(arCls);
		return LSOK;
	}
	template <class Type>
	LSError Serialize(const RStringB &name, StaticArray< Ref<Type> > &value, int minVersion)
	{
		if (_version < minVersion) return LSOK;
	// call Serialize for both passes
		ParamArchive arCls;
		int n;
		if (_saving)
		{
			n = value.Size();
			if( n==0 ) return LSOK;

			if (!OpenSubclass(name, arCls)) ON_ERROR(LSNoEntry, name);

			PARAM_CHECK(arCls.Serialize("items", n, minVersion))
		}
		else if (_pass == PassSecond)
		{
			n = value.Size();
			if (!OpenSubclass(name, arCls))
			{
				if( n==0 ) return LSOK;
				ON_ERROR(LSNoEntry, name);
			}
		}
		else
		{
			if (!OpenSubclass(name, arCls))
			{
				value.Clear();
				return LSOK;
			}
			PARAM_CHECK(arCls.Serialize("items", n, minVersion))
			value.Resize(n);
		}
		for (int i=0; i<n; i++)
		{
			char buffer[256];
			snprintf(buffer, sizeof(buffer), "Item%d", i);
			PARAM_CHECK(arCls.Serialize(buffer, value[i], minVersion))
		}
		CloseSubclass(arCls);
		return LSOK;
	}
	template <class Type>
	LSError SerializeRefs(const RStringB &name, RefArray<Type> &value, int minVersion)
	{
		if (_version < minVersion) return LSOK;
		if (!_saving && _pass != PassSecond) return LSOK;
		ParamArchive arCls;
		int n;
		if (_saving)
		{
			n = value.Size();
			if( n==0 ) return LSOK;
			if (!OpenSubclass(name, arCls)) ON_ERROR(LSNoEntry, name);
			PARAM_CHECK(arCls.Serialize("items", n, minVersion))
		}
		else
		{
			if (!OpenSubclass(name, arCls))
			{
				value.Clear();
				return LSOK;
			}
			// cannot use serialize - second pass doesn's serialize int
			ParamEntry *entry = arCls._entry->FindEntry("items");
			if (!entry) ON_ERROR(LSNoEntry, name + RString(".items"));
			n = *entry;
			value.Resize(n);
		}
		for (int i=0; i<n; i++)
		{
			char buffer[256];
			snprintf(buffer, sizeof(buffer), "Item%d", i);
			PARAM_CHECK(arCls.SerializeRef(buffer, value[i], minVersion))
		}
		CloseSubclass(arCls);
		return LSOK;
	}
	template <class Type>
	LSError SerializeRefs(const RStringB &name, LinkArray<Type> &value, int minVersion)
	{
		if (_version < minVersion) return LSOK;
		if (!_saving && _pass != PassSecond) return LSOK;
		ParamArchive arCls;
		int n;
		if (_saving)
		{
			n = value.Size();
			if( n==0 ) return LSOK;
			if (!OpenSubclass(name, arCls)) ON_ERROR(LSNoEntry, name);
			PARAM_CHECK(arCls.Serialize("items", n, minVersion))
		}
		else
		{
			if (!OpenSubclass(name, arCls))
			{
				value.Clear();
				return LSOK;
			}
			// cannot use serialize - second pass doesn's serialize int
			ParamEntry *entry = arCls._entry->FindEntry("items");
			if (!entry) ON_ERROR(LSNoEntry, name + RString(".items"));
			n = *entry;
			value.Resize(n);
		}
		for (int i=0; i<n; i++)
		{
			char buffer[256];
			snprintf(buffer, sizeof(buffer), "Item%d", i);
			PARAM_CHECK(arCls.SerializeRef(buffer, value[i], minVersion))
		}
		CloseSubclass(arCls);
		return LSOK;
	}
	template <class Type>
	LSError SerializeRefs(const RStringB &name, LLinkArray<Type> &value, int minVersion)
	{
		if (_version < minVersion) return LSOK;
		if (!_saving && _pass != PassSecond) return LSOK;
		ParamArchive arCls;
		int n;
		if (_saving)
		{
			n = value.Size();
			if( n==0 ) return LSOK;
			if (!OpenSubclass(name, arCls)) ON_ERROR(LSNoEntry, name);
			PARAM_CHECK(arCls.Serialize("items", n, minVersion))
		}
		else
		{
			if (!OpenSubclass(name, arCls))
			{
				value.Clear();
				return LSOK;
			}
			// cannot use serialize - second pass doesn's serialize int
			ParamEntry *entry = arCls._entry->FindEntry("items");
			if (!entry) ON_ERROR(LSNoEntry, name + RString(".items"));
			n = *entry;
			value.Resize(n);
		}
		for (int i=0; i<n; i++)
		{
			char buffer[256];
			snprintf(buffer, sizeof(buffer), "Item%d", i);
			PARAM_CHECK(arCls.SerializeRef(buffer, value[i], minVersion))
		}
		CloseSubclass(arCls);
		return LSOK;
	}
};

template <class Type>
LSError Serialize(ParamArchive &ar, const RStringB &name, Type &value, int minVersion, Type defValue)
{
	if (ar.GetArVersion() < minVersion)
	{
		if (ar.IsLoading() && ar.GetPass() == ParamArchive::PassFirst) value = defValue;
		return LSOK;
	}
	if (ar.IsSaving() && value == defValue) return LSOK;
	LSError err = Serialize(ar, name, value, minVersion);
	if (err == LSNoEntry)
	{
		value = defValue;
		CANCEL_ERROR;
	}
	return err;
}

namespace Poseidon { class ParamFile; }
using Poseidon::ParamFile;
class ParamArchiveLoad : public ParamArchive
{
protected:
	bool _open;
	SRef<ParamFile> _file;

public:
	ParamArchiveLoad();
	ParamArchiveLoad(const char *name, void *params = nullptr);
	~ParamArchiveLoad() override;

	ParamFile *GetParamFile();
	LSError Load(const char *name);
	bool LoadBin(const char *name);
	void Close();
};

class ParamArchiveSave : public ParamArchive
{
protected:
	bool _saved;
	SRef<ParamFile> _file;

public:
	ParamArchiveSave(int version, void *params = nullptr);
	~ParamArchiveSave() override;

	ParamFile *GetParamFile();

	LSError Parse(const char *name);
	LSError Save(const char *name);
	bool ParseBin(const char *name);
	bool SaveBin(const char *name);
};
