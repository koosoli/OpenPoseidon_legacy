// Reforger parity capture (Open Poseidon, MrDudeX-reforger): drives the World Editor camera
// through the pose list in MDX_ParityPoses.c and saves one screenshot per pose, so Open
// Poseidon's captures of the same poses can be compared with the original.
//
// Written against the public Enfusion/Arma Reforger script API only (WorldEditorAPI.SetCamera,
// GetTerrainSurfaceY, System.MakeScreenshot, BaseWeatherManagerEntity.SetTimeOfTheDay).
// The entity is created by MDX_ParityCapturePlugin and is never saved into a world.

class MDX_ParityPose
{
	string m_sName;
	float m_fX;
	float m_fZ;
	float m_fAlt;
	bool m_bAbsolute;
	float m_fAzimuth;
	float m_fElevation;

	void MDX_ParityPose(string name, float x, float z, float alt, bool absolute, float azimuth, float elevation)
	{
		m_sName = name;
		m_fX = x;
		m_fZ = z;
		m_fAlt = alt;
		m_bAbsolute = absolute;
		m_fAzimuth = azimuth;
		m_fElevation = elevation;
	}
}

[EntityEditorProps(category: "MDX", description: "Parity capture camera driver (temporary)")]
class MDX_ParityCamEntityClass : GenericEntityClass
{
}

class MDX_ParityCamEntity : GenericEntity
{
#ifdef WORKBENCH
	protected ref array<ref MDX_ParityPose> m_aPoses;
	protected int m_iIndex = -1;
	protected float m_fClock;
	protected int m_iPhase; // 0 = start, 1 = settling, 2 = shot taken, 3 = done
	protected float m_fHour = 11;
	protected float m_fSettle = 12;
	protected float m_fFirstSettle = 40;
	protected float m_fFovY = 55.4;
	protected string m_sOut = "$profile:MDXParity";
	protected bool m_bExit = true;

	protected void Log(string text)
	{
		Print("[MDXParity] " + text);
		FileHandle f = FileIO.OpenFile(m_sOut + "/capture.log", FileMode.APPEND);
		if (f)
		{
			f.WriteLine(text);
			f.Close();
		}
	}

	protected TimeAndWeatherManagerEntity m_TW;

	// The sun and the calendar the shot was taken under, so the other engine can be set to the
	// same light (BaseWeatherManagerEntity.GetCurrentSunMoonDirAndPhase / GetDate / latitude).
	protected void LogSun(string poseName)
	{
		if (!m_TW)
			return;
		vector sunDir, moonDir;
		float moonPhase;
		m_TW.GetCurrentSunMoonDirAndPhase(sunDir, moonDir, moonPhase);
		int y, mo, d;
		m_TW.GetDate(y, mo, d);
		Log(string.Format("sun %1: dir %2 date %3-%4-%5 tod %6 lat %7 lon %8 tz %9", poseName, sunDir, y, mo, d,
			m_TW.GetTimeOfTheDay(), m_TW.GetCurrentLatitude(), m_TW.GetCurrentLongitude(), m_TW.GetTimeZoneOffset()));
	}

	// The water surface under the camera (SCR_WorldTools.GetWaterSurfaceY), so lake and river
	// levels can be compared with the other engine's.
	protected void LogWater(MDX_ParityPose pose)
	{
		WorldEditorAPI api = _WB_GetEditorAPI();
		if (!api)
			return;
		float ground = api.GetTerrainSurfaceY(pose.m_fX, pose.m_fZ);
		vector at = Vector(pose.m_fX, ground + 0.01, pose.m_fZ);
		EWaterSurfaceType type;
		float lakeArea;
		float waterY = SCR_WorldTools.GetWaterSurfaceY(GetWorld(), at, type, lakeArea);
		Log(string.Format("water %1: x %2 z %3 ground %4 water %5 type %6 lakeArea %7", pose.m_sName, pose.m_fX,
			pose.m_fZ, ground, waterY, type, lakeArea));
	}

	protected void Setup()
	{
		m_aPoses = {};
		MDX_ParityPoses.Fill(m_aPoses);
		m_fHour = MDX_ParityPoses.Hour();
		m_fSettle = MDX_ParityPoses.Settle();
		m_fFirstSettle = MDX_ParityPoses.FirstSettle();
		m_fFovY = MDX_ParityPoses.FovY();
		m_bExit = MDX_ParityPoses.ExitWhenDone();
		FileIO.MakeDirectory(m_sOut);
		Log(string.Format("start: %1 poses, hour %2, settle %3 s", m_aPoses.Count(), m_fHour, m_fSettle));

		BaseWorld world = GetWorld();
		ChimeraWorld chimera = ChimeraWorld.CastFrom(world);
		if (chimera)
		{
			TimeAndWeatherManagerEntity tw = chimera.GetTimeAndWeatherManager();
			if (tw)
			{
				tw.SetIsDayAutoAdvanced(false);
				tw.SetTimeOfTheDay(m_fHour, true);
				tw.SetDateTimePreview(true, -1, -1, -1, m_fHour);
				tw.SetRainIntensityOverride(true, 0);
				tw.SetFogAmountOverride(true, 0);
				tw.SetWindSpeedOverride(true, 2);
				m_TW = tw;
				Log(string.Format("time of day %1 set", m_fHour));
			}
			else
				Log("no TimeAndWeatherManagerEntity");
		}
		else
			Log("world is not a ChimeraWorld; time of day left as is");
	}

	protected void Aim(WorldEditorAPI api, MDX_ParityPose pose)
	{
		float y = pose.m_fAlt;
		if (!pose.m_bAbsolute)
			y += api.GetTerrainSurfaceY(pose.m_fX, pose.m_fZ);
		float az = pose.m_fAzimuth * Math.DEG2RAD;
		float el = pose.m_fElevation * Math.DEG2RAD;
		vector dir = Vector(Math.Sin(az) * Math.Cos(el), Math.Sin(el), Math.Cos(az) * Math.Cos(el));
		vector pos = Vector(pose.m_fX, y, pose.m_fZ);
		api.SetCamera(pos, dir);
		BaseWorld world = GetWorld();
		if (world && m_fFovY > 1)
			world.SetCameraVerticalFOV(world.GetCurrentCameraId(), m_fFovY);
		Log(string.Format("pose %1 at %2 dir %3", pose.m_sName, pos, dir));
	}

	override int _WB_GetAfterWorldUpdateSpecs(IEntitySource src)
	{
		return EEntityFrameUpdateSpecs.CALL_ALWAYS;
	}

	override void _WB_AfterWorldUpdate(float timeSlice)
	{
		WorldEditorAPI api = _WB_GetEditorAPI();
		if (!api || m_iPhase == 3)
			return;
		if (!m_aPoses)
		{
			Setup();
			m_iIndex = 0;
			m_iPhase = 1;
			m_fClock = 0;
			if (m_aPoses.IsEmpty())
			{
				m_iPhase = 3;
				return;
			}
			Aim(api, m_aPoses[0]);
			return;
		}
		m_fClock += timeSlice;
		MDX_ParityPose pose = m_aPoses[m_iIndex];
		float settle = m_fSettle;
		if (m_iIndex == 0)
			settle = m_fFirstSettle;
		if (m_iPhase == 1)
		{
			// Keep re-applying the pose: the editor camera may be nudged by its own input.
			Aim(api, pose);
			if (m_fClock >= settle)
			{
				string path = m_sOut + "/" + pose.m_sName;
				LogSun(pose.m_sName);
				LogWater(pose);
				bool ok = System.MakeScreenshot(path);
				Log(string.Format("shot %1 -> %2 (%3)", pose.m_sName, path, ok));
				m_iPhase = 2;
				m_fClock = 0;
			}
			return;
		}
		if (m_iPhase == 2 && m_fClock >= 1.5)
		{
			m_iIndex++;
			m_fClock = 0;
			if (m_iIndex >= m_aPoses.Count())
			{
				m_iPhase = 3;
				Log("done");
				FileHandle f = FileIO.OpenFile(m_sOut + "/DONE", FileMode.WRITE);
				if (f)
				{
					f.WriteLine("done");
					f.Close();
				}
				if (m_bExit)
					Workbench.Exit(0);
				return;
			}
			m_iPhase = 1;
			Aim(api, m_aPoses[m_iIndex]);
		}
	}
#endif
}
