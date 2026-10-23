#pragma once

#include <Poseidon/Foundation/Time/Time.hpp>
#include <Poseidon/Network/NetworkObject.hpp>
#include <Poseidon/Foundation/Types/RemoveLinks.hpp>
#include <Poseidon/IO/Serialization/ParamArchive.hpp>
namespace Poseidon { class IWave; }
namespace Poseidon { class ParamEntry; }

// ChatChannel (Game) and SentenceParams (UI) are defined at global scope in
// not-yet-wrapped subsystems; forward-declare them there, not inside Poseidon.
DECL_ENUM(ChatChannel)
namespace Poseidon { class SentenceParams; }
using Poseidon::SentenceParams;

namespace Poseidon
{
class BasicSpeaker: public RefCount
{
	AutoArray<RString> _directories;
	// --arma2: an Arma 2 voice (CfgVoice >> Male01EN of the Arma 2 config) speaks its own
	// protocol's words from its own directories; _directories stay the CWA voice that says
	// a word Arma 2's radio has none for.
	AutoArray<RString> _a2Directories;
	const ParamEntry *_a2Words = nullptr;
	RString A2Clip(const RString &id) const;
	
	public:
	BasicSpeaker( const ParamEntry &cfg );
	BasicSpeaker( const ParamEntry &cwaFallback, const ParamEntry &a2Voice, const ParamEntry &a2Words );
	bool IsArma2() const {return _a2Words!=nullptr;}
	IWave *Say(RString id, float pitch, bool loop);
};

class Speaker
{
	Ref<BasicSpeaker> _basic;
	float _pitch;

	public:
	Speaker(){}
	Speaker( Ref<BasicSpeaker> basic, float pitch )
	:_basic(basic),_pitch(pitch)
	{}

	bool IsSpeakerValid() const {return _basic!=nullptr;}
	IWave *Say( RString id ) {return _basic->Say(id,_pitch,false);}
	IWave *SayNoPitch( RString id, bool loop ) {return _basic->Say(id,1,loop);}
};

class RadioChannel;

class RadioMessage: public RefCountWithLinks
{
protected:
	int _language;
	int _priority;
	Poseidon::Foundation::Time _timeOut;
	bool _transmitted;
	bool _playerMsg;

public:
	RadioMessage();
	~RadioMessage() override {}
	bool IsTransmitted() const {return _transmitted;}
	void SetTransmitted() {_transmitted = true;}
	bool IsPlayerMsg() const {return _playerMsg;}
	void SetPlayerMsg(bool set = true) {_playerMsg = set;}

	virtual void Transmitted() {} // message completed
	virtual void Canceled() {} // message canceled

	virtual float GetDuration() const;
	virtual int GetType() const = 0;
	int GetLanguage() const {return _language;}
	void SetLanguage(int language) {_language = language;}
	int GetPriority() const {return _priority;}
	void SetPriority(int priority) {_priority = priority;}
	virtual const char *GetPriorityClass() = 0;

	Poseidon::Foundation::Time GetTimeOut() const {return _timeOut;}
	void SetTimeOut( Poseidon::Foundation::Time time ) {_timeOut=time;}

	virtual const char *PrepareSentence(SentenceParams &params) = 0;
	virtual AIUnit *GetSender() const = 0;
	virtual Speaker *GetSpeaker() const;
	
	virtual RString GetText() {return "";}
	virtual RString GetWave() {return "";}
	virtual RString GetSenderName() {return "";}

	virtual LSError Serialize(ParamArchive &ar);
	static RadioMessage *CreateObject(ParamArchive &ar);
};

struct RadioWord
{
	RString id;
	float pauseAfter;
};

class RadioSentence : public AutoArray<RadioWord>
{
public:
	int Add(RString id, float pauseAfter)
	{
		int index = AutoArray<RadioWord>::Add();
		Set(index).id = id;
		Set(index).pauseAfter = pauseAfter;
		return index;
	};
	void Say(RadioChannel *channel, Speaker *speaker);
};

enum RadioNoise
{ // noise type - 
	RNRadio, // air 
	RNIntercomm, // intercomm
	RNNone // no noise
};

class RadioChannel : public RemoveLinks, public SerializeClass
{
	bool _audible; // some channels are only virtual - no acoustics, no words
	ChatChannel _chatChannel;
	OLink<NetworkObject> _object;

	Ref<IWave> _saying; // queue to this wave
	Ref<IWave> _noise; // noise channel
	Speaker _speaker;
	RadioNoise _noiseType;

	float _pauseAfter;
	float _pauseAfterMessage;
	Ref<RadioMessage> _actualMsg;
	RefArray<RadioMessage> _messageQueue;

	// messageQueue holds all waiting messages
	// active message is converted to words and added to word queue
	
	void NextMessage();

	RadioMessage *CreateMessage(int type);

	public:
	RadioChannel(ChatChannel chatChannel, NetworkObject *object, RadioNoise noise);
	void Simulate( float deltaT );
	//! process all waiting messages as transmitted
	void SilentProcess();
	bool Done() const;
	void Say(Speaker *speaker, RString id, float pauseAfter, bool transmit);
	void Say(RString waveName, AIUnit *sender, RString senderName, RString player, float duration);
	void Transmit(RadioMessage *msg, int language);
	void Cancel( RadioMessage *msg );
	void Replace( RadioMessage *msg, RadioMessage *with );

	void CancelAllMessages();

	bool IsAudible() const {return _audible;}
	void SetAudible( bool audible );

	bool IsEmpty() const {return !_actualMsg && _messageQueue.Size() == 0;}
	bool IsSilent() const;

	LSError Serialize(ParamArchive &ar) override;

	RadioMessage *FindNextMessage(int type, int& index) const; // first for index < 0
	RadioMessage *FindPrevMessage(int type, int& index) const; // last for index >= NMessages
	RadioMessage *GetNextMessage(int& index) const; // first for index < 0
	RadioMessage *GetPrevMessage(int& index) const; // last for index >= NMessages
	RadioMessage *GetActualMessage() const {return _actualMsg;}
};



} // namespace Poseidon
