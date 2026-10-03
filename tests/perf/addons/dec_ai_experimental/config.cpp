// Optional author-test configuration. Not part of the default installation.
// These mechanisms retain known test failures; restart to change configuration.
class CfgPatches
{
    class OP_DecAiExperimental
    {
        units[] = {};
        weapons[] = {};
        requiredVersion = 1.0;
        requiredAddons[] = {};
    };
};
class CfgAIFork
{
    gradualSpotting = 1;
    hunting = 1;
    grenadeInterval = 5;
    exposedParts = 1;
};
