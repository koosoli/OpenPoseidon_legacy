#pragma once

#include <Poseidon/Foundation/Strings/RString.hpp>
#include <Poseidon/Foundation/Time/Time.hpp>
#include <Poseidon/Foundation/Containers/Array.hpp>
#include <Evaluator/express.hpp>

namespace Poseidon
{


struct ScriptLine
{
	RString waitUntil;
	RString suspendUntil; // wait until time reach this value
	RString condition; // skip if condition not met
	RString statement;

	LSError Serialize(ParamArchive &ar);
};

struct ScriptLabel
{
	RString name;
	int line;

	LSError Serialize(ParamArchive &ar);
};

}  // namespace Poseidon

#include <Poseidon/Foundation/Types/LLinks.hpp>

namespace Poseidon
{

struct ScriptFiber; // Scripts.cpp: Arma 2 scheduled code running on its own stack

class Script : public RemoveLLinks
{
	friend struct ScriptFiber;
protected:
	ScriptFiber *_fiber = nullptr;
	GameValue _argument;
	Poseidon::Foundation::Time _baseTime;
	int _currentLine;
	AutoArray<ScriptLine> _lines;
	AutoArray<ScriptLabel> _labels;
	GameVarSpace _vars;
	float _time;
	float _suspendedUntil;
	int _maxLinesAtOnce;
	RString _name;
	int _handle = 0;

public:
	Script();
	Script(RString name, GameValuePar argument, int maxLines=-1);
	Script(const AutoArray<RString> &lines, GameValuePar argument, int maxLines=-1);
	~Script() override;

	// Scheduled SQF (spawn / execVM): `code` is split into its top-level statements;
	// a top-level `waitUntil {..}` or `sleep n` suspends the script exactly like SQS
	// '@' and '~'. Suspension nested inside other statements is not available and is
	// reported once. Returns a script with a fresh handle (see GetHandle).
	static Script *CreateFromCode(RString name, RString code, GameValuePar argument);
	// Arma 2: the same as a coroutine that may suspend anywhere (see ScriptFiber).
	static Script *CreateFiberScript(RString name, RString code, GameValuePar argument);
	int GetHandle() const {return _handle;}
	static Script *FindByHandle(int handle);

	RString GetDebugName() const {return _name;}
	void SetName(RString name) {_name=name;}
#if POSEIDON_DIAG
	// DIAG-001: diag_scripts
	int DiagLine() const {return _currentLine;}
	int DiagNLines() const {return _lines.Size();}
	const char *DiagStatement(int i) const {return i >= 0 && i < _lines.Size() ? (const char *)_lines[i].statement : "";}
	float DiagTime() const {return _time;}
#endif

	LSError Serialize(ParamArchive &ar);
	static Script *CreateObject(ParamArchive &ar) {return new Script();}
	
	bool SimulateBody();
	bool SimulateFiber();
	bool OnSimulate();
	bool Simulate(float deltaT);
	bool IsTerminated() const;
	void GoTo(RString label);
	void Exit();

	GameValue VarGet(const char *name) const;
	void VarSet(const char *name, GameValuePar value, bool readOnly = false);

protected:
	void ProcessLine(const char *line);
};
}  // namespace Poseidon
