// Reforger parity capture: opens Everon in the World Editor (when no world is open) and adds the
// temporary MDX_ParityCamEntity that drives the camera through the pose list. Run it from the
// Plugins menu, or from the command line:
//   ArmaReforgerWorkbenchSteamDiag.exe -gproj <...>\MDXParity\addon.gproj -wbModule=WorldEditor -run -plugin=MDX_ParityCapturePlugin
// Nothing is saved: the world comes from the read-only game data and the entity only lives in
// the open session.
[WorkbenchPluginAttribute(name: "MDX Parity Capture", wbModules: {"WorldEditor"}, awesomeFontCode: 0xf030)]
class MDX_ParityCapturePlugin : WorldEditorPlugin
{
	protected static const string WORLD = "{853E92315D1D9EFE}worlds/Eden/Eden.ent";

	override void Run()
	{
		Start();
	}

	override void RunCommandline()
	{
		Start();
	}

	protected void Start()
	{
		WorldEditor we = Workbench.GetModule(WorldEditor);
		if (!we)
		{
			Print("[MDXParity] no World Editor module", LogLevel.ERROR);
			return;
		}
		WorldEditorAPI api = we.GetApi();
		string path;
		api.GetWorldPath(path);
		if (path.IsEmpty() || !path.Contains("Eden.ent"))
		{
			Print("[MDXParity] opening " + WORLD);
			we.SetOpenedResource(WORLD);
			api = we.GetApi();
			api.GetWorldPath(path);
		}
		Print("[MDXParity] world: " + path);
		int subScene = api.GetCurrentSubScene();
		int layer = api.GetSubsceneLayerId(subScene, api.GetActiveSubsceneLayer(subScene));
		api.BeginEntityAction("MDX parity capture");
		IEntitySource src = api.CreateEntity("MDX_ParityCamEntity", "MDX_ParityCam", layer, null, "0 0 0", "0 0 0");
		api.EndEntityAction();
		if (src)
			Print("[MDXParity] capture entity created");
		else
			Print("[MDXParity] could not create the capture entity", LogLevel.ERROR);
	}
}
