#pragma once

// (OP) Tidewater's humpback whale (@OP_TidewaterWildlife, simulation "whale"): a port of Tidewater's
// WhaleBrain.js (route, surfacing sequences, blows, breaches, flipper slaps, the fluke-up dive) and of the spine
// rig Whale.js poses in its vertex shader, here on the CPU in Animate. See Whale.cpp.

#include <Poseidon/World/Scene/Thing.hpp>

#include <memory>
#include <mutex>

namespace Poseidon
{
class Whale: public Thing
{
	typedef Thing base;

	public:
	struct Brain;
	struct Rig;
	struct Pose;

	Whale(VehicleType *type);
	~Whale() override;

	void Simulate(float deltaT, SimulationImportance prec) override;
	void HitBy(EntityAI *owner, float howMuch, RString ammo) override;
	// (OP) a hit bleeds into the water where it struck; an explosion tears pieces off
	void DoDammage(EntityAI *owner, Vector3Par pos, float val, float valRange, RString ammo) override;

	bool IsAnimated(int level) const override {return true;}
	bool IsAnimatedShadow(int level) const override {return true;}
	void Animate(int level) override;
	void Deanimate(int level) override;

	void Sound(bool inside, float deltaT) override {}
	void UnloadSound() override {}

	USE_CASTING(base)

	private:
	void Start();
	float WaterLevel() const;
	void Effects(float deltaT);
	void Publish();

	std::unique_ptr<Brain> _brain;
	std::shared_ptr<const Rig> _rig;
	// the pose Animate draws (written by Simulate, read by the renderer: under _poseLock)
	std::unique_ptr<Pose> _pose;
	mutable std::mutex _poseLock;
	float _lastDammage = 0;
	int _slapsSeen = 0, _splashesSeen = 0;
	bool _blowSent = false;
	float _logNext = 0;
	void Bleed(Vector3Par at, float amount);
	void TearChunks(Vector3Par at, float val);
	float _bleed = 0; // blood the wounds still lose (stain amount a second), dying away
	float _bleedClock = 0;
};

} // namespace Poseidon
