//! Game instance management — spawn game binary with `--harness`, connect client.

use super::connection::ClientConfig;
use super::HarnessClient;
use crate::protocol::Event;
use anyhow::{Context, Result};
use std::path::{Path, PathBuf};
use std::process::Stdio;
use std::sync::{Arc, Mutex};
use std::time::Duration;
use tokio::io::AsyncBufReadExt;
use tokio::process::{Child, Command};

#[derive(Default)]
struct LogTapState {
    lines: Vec<String>,
    bytes: usize,
    overflowed: bool,
}

impl LogTapState {
    fn push(&mut self, line: String) {
        if self.overflowed
            || self.lines.len() >= 65536
            || self.bytes.saturating_add(line.len()) > 16 * 1024 * 1024
        {
            self.overflowed = true;
            return;
        }
        self.bytes += line.len();
        self.lines.push(line);
    }

    fn since(&self, mark: usize) -> Result<Vec<String>> {
        anyhow::ensure!(
            !self.overflowed,
            "stdout assertion tap overflowed; complete file remains separate"
        );
        Ok(self
            .lines
            .get(mark..)
            .context("stdout assertion mark exceeds captured lines")?
            .to_vec())
    }
}

type LogTap = Arc<Mutex<LogTapState>>;

/// PIDs of live game processes spawned by this tri run.  Consulted by the
/// Ctrl-C handler in `main` so an interrupted run can reap its children —
/// process Drop glue (`kill_on_drop`) never runs when the process is torn
/// down by a console interrupt, which is exactly when a long serial run
/// gets abandoned and would otherwise strand every in-flight game.
static LIVE_GAME_PIDS: std::sync::Mutex<Vec<u32>> = std::sync::Mutex::new(Vec::new());

fn register_pid(pid: u32) {
    if let Ok(mut pids) = LIVE_GAME_PIDS.lock() {
        pids.push(pid);
    }
}

fn unregister_pid(pid: u32) {
    if let Ok(mut pids) = LIVE_GAME_PIDS.lock() {
        pids.retain(|&p| p != pid);
    }
}

/// Synchronously kill every still-registered game process (and, on Windows,
/// its whole process tree).  Called from the Ctrl-C handler — must not rely
/// on the tokio runtime still driving child futures, so it shells out.
pub fn kill_all_registered_games() {
    let pids: Vec<u32> = LIVE_GAME_PIDS.lock().map(|p| p.clone()).unwrap_or_default();
    for pid in pids {
        kill_tree_by_pid_blocking(pid);
        unregister_pid(pid);
    }
}

/// Best-effort synchronous kill of `pid` and its descendants.
fn kill_tree_by_pid_blocking(pid: u32) {
    #[cfg(windows)]
    {
        // `taskkill /T` walks the child-process tree; /F is TerminateProcess.
        let _ = std::process::Command::new("taskkill")
            .args(["/PID", &pid.to_string(), "/T", "/F"])
            .stdin(std::process::Stdio::null())
            .stdout(std::process::Stdio::null())
            .stderr(std::process::Stdio::null())
            .status();
    }
    #[cfg(not(windows))]
    {
        let _ = std::process::Command::new("kill")
            .args(["-KILL", &pid.to_string()])
            .stdin(std::process::Stdio::null())
            .stdout(std::process::Stdio::null())
            .stderr(std::process::Stdio::null())
            .status();
    }
}

/// Kill `process` (and on Windows its whole process tree), then *reap* it so
/// the caller returns only once the OS has actually torn the game down.
///
/// This is the load-bearing cleanup path.  A dropped tokio `Child` does NOT
/// reliably kill the process: `kill_on_drop` only issues a fire-and-forget
/// `start_kill()`, which on Windows can be outrun by tri exiting — and it
/// never touches grandchildren.  Every early-return that abandons a game
/// (handshake timeout, connect failure, scenario timeout) must come through
/// here, not through Drop.
pub async fn kill_process_tree(process: &mut Child) {
    // Grab the PID before try_wait — a reaped Child reports id() = None.
    let pid_before = process.id();
    // Already exited and reaped — nothing to kill (and the PID may have been
    // recycled, so a taskkill here would be actively dangerous).
    if matches!(process.try_wait(), Ok(Some(_))) {
        if let Some(pid) = pid_before {
            unregister_pid(pid);
        }
        return;
    }
    if let Some(pid) = pid_before {
        // Tree kill first so children die before the root's PID is freed.
        let pid_s = pid.to_string();
        #[cfg(windows)]
        let mut cmd = {
            let mut c = Command::new("taskkill");
            c.args(["/PID", &pid_s, "/T", "/F"]);
            c
        };
        #[cfg(not(windows))]
        let mut cmd = {
            let mut c = Command::new("kill");
            c.args(["-KILL", &pid_s]);
            c
        };
        let _ = tokio::time::timeout(
            Duration::from_secs(10),
            cmd.stdin(Stdio::null())
                .stdout(Stdio::null())
                .stderr(Stdio::null())
                .status(),
        )
        .await;
        unregister_pid(pid);
    }
    // Direct kill as backstop (taskkill missing/failed, or races), then reap
    // so the process object is gone before we return.
    let _ = process.kill().await;
    let _ = tokio::time::timeout(Duration::from_secs(5), process.wait()).await;
}

/// A running game instance with a connected harness client.
pub struct GameInstance {
    process: Child,
    client: Option<HarnessClient>,
    port: u16,
    game_binary: PathBuf,
    work_dir: PathBuf,
    config: ClientConfig,
    log_tap: Option<LogTap>, // Only auto-port instances have a drained stdout pipe.
}

impl GameInstance {
    pub fn log_mark(&self) -> Result<usize> {
        let tap = self
            .log_tap
            .as_ref()
            .context("stdout assertions require an auto-port instance")?;
        let tap = tap
            .lock()
            .map_err(|_| anyhow::anyhow!("stdout assertion tap poisoned"))?;
        anyhow::ensure!(!tap.overflowed, "stdout assertion tap overflowed");
        Ok(tap.lines.len())
    }

    pub fn log_lines_since(&self, mark: usize) -> Result<Vec<String>> {
        let tap = self
            .log_tap
            .as_ref()
            .context("stdout assertions require an auto-port instance")?;
        tap.lock()
            .map_err(|_| anyhow::anyhow!("stdout assertion tap poisoned"))?
            .since(mark)
    }
    /// Spawn a game instance with `--harness <port>` and optional extra args.
    /// Uses the provided [`ClientConfig`] for connection timeouts and retry behaviour.
    ///
    /// `game_dir` is where the binary lives. If game data is in a different
    /// directory, pass `data_dir` — the process will use `-C <data_dir>` to set
    /// the game's working directory.
    pub async fn spawn(
        game_dir: &str,
        port: u16,
        extra_args: &[&str],
        config: &ClientConfig,
    ) -> Result<Self> {
        Self::spawn_with_data(game_dir, None, port, extra_args, config).await
    }

    /// Like [`Self::spawn`] but with an explicit data directory and environment overrides.
    pub async fn spawn_with_data(
        game_dir: &str,
        data_dir: Option<&str>,
        port: u16,
        extra_args: &[&str],
        config: &ClientConfig,
    ) -> Result<Self> {
        Self::spawn_with_env(game_dir, data_dir, port, extra_args, &[], None, config).await
    }

    /// Full-featured spawn: data directory, extra args, environment variable overrides,
    /// and optional binary-name preference.
    pub async fn spawn_with_env(
        game_dir: &str,
        data_dir: Option<&str>,
        port: u16,
        extra_args: &[&str],
        env_vars: &[(&str, &str)],
        binary_name: Option<&str>,
        config: &ClientConfig,
    ) -> Result<Self> {
        let game_dir = std::path::absolute(Path::new(game_dir))
            .with_context(|| format!("failed to resolve game_dir '{game_dir}'"))?;
        let binary = find_game_binary(&game_dir, binary_name)?;

        let work_dir = data_dir.map_or_else(|| game_dir.clone(), PathBuf::from);
        let work_dir = std::path::absolute(&work_dir)
            .with_context(|| format!("failed to resolve work_dir '{}'", work_dir.display()))?;

        let mut cmd = Command::new(&binary);
        cmd.arg("--harness").arg(port.to_string()); // port=0 → game auto-assigns

        // If data_dir differs from game_dir, pass -C to set game working directory
        if data_dir.is_some() {
            cmd.arg("-C").arg(&work_dir);
        }

        for &(key, val) in env_vars {
            cmd.env(key, val);
        }

        // When port=0 the game picks its own port and announces it on stdout.
        // We must pipe stdout to read that announcement; then save remainder to log.
        let output_dir = env_vars
            .iter()
            .find(|(k, _)| *k == "TRI_OUTPUT_DIR")
            .map(|(_, v)| std::path::Path::new(v).to_path_buf());

        let auto_port = port == 0;

        // Auto-port instances used to share one `game_stdout.log`, which is fine
        // serially but destroys the logs under `-j`: every concurrent game
        // truncates the same file and interleaves into it, so the run loses its
        // diagnostics exactly when a parallel failure needs explaining. Number
        // them instead. The port-pinned branch below is already unique per port.
        static INSTANCE_SEQ: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);
        let seq = INSTANCE_SEQ.fetch_add(1, std::sync::atomic::Ordering::Relaxed);

        let stdout_log = output_dir.as_ref().and_then(|d| {
            let _ = std::fs::create_dir_all(d);
            std::fs::File::create(d.join(format!("game_stdout_{seq}.log"))).ok()
        });

        let (stdout_cfg, stderr_cfg) = if auto_port {
            let stderr = output_dir.as_ref().and_then(|d| {
                let _ = std::fs::create_dir_all(d);
                std::fs::File::create(d.join(format!("game_stderr_{seq}.log"))).ok()
            });
            (Stdio::piped(), stderr.map_or_else(Stdio::null, Stdio::from))
        } else {
            let log = output_dir.as_ref().and_then(|d| {
                let _ = std::fs::create_dir_all(d);
                std::fs::File::create(d.join(format!("game_{port}.log"))).ok()
            });
            let stderr = log.as_ref().and_then(|f| f.try_clone().ok());
            (
                log.map_or_else(Stdio::null, Stdio::from),
                stderr.map_or_else(Stdio::null, Stdio::from),
            )
        };

        cmd.args(extra_args)
            .current_dir(&work_dir)
            .stdin(Stdio::null())
            .stdout(stdout_cfg)
            .stderr(stderr_cfg)
            // Kill the game synchronously when the GameInstance Drop
            // runs.  Without this, retried/timed-out tests leak game
            // processes: Drop's `start_kill()` is fire-and-forget on
            // Windows (TerminateProcess is queued, kernel may process
            // it after tri exits), so the game survives as a parentless
            // orphan with stdio handles still open.  For that reason this
            // is a LAST-RESORT backstop only, not the cleanup path: every
            // deliberate teardown (handshake timeout, connect failure,
            // scenario failure/timeout, Ctrl-C) goes through
            // kill_process_tree, which kills tree-wide, awaits, and reaps.
            .kill_on_drop(true);

        tracing::debug!(
            "Spawning {} --harness {port} {}",
            binary.display(),
            extra_args.join(" ")
        );
        let mut process = cmd.spawn().map_err(|e| {
            anyhow::anyhow!(
                "failed to spawn game binary '{}' (cwd '{}'): {e}",
                binary.display(),
                work_dir.display()
            )
        })?;
        if let Some(pid) = process.id() {
            register_pid(pid);
        }

        let log_tap = auto_port.then(|| Arc::new(Mutex::new(LogTapState::default())));
        let actual_port = if auto_port {
            match read_harness_port(
                &mut process,
                stdout_log,
                log_tap.as_ref().unwrap().clone(),
                config.connect_timeout,
            )
            .await
            {
                Ok(p) => p,
                Err(e) => {
                    // The game is still running (or wedged) — kill it and its
                    // tree *before* reporting the handshake failure.  Relying
                    // on Drop here is the zombie-leak bug: kill_on_drop's
                    // start_kill is fire-and-forget and tree-blind, and over
                    // a long run (worse with --retries) the orphans pile up.
                    kill_process_tree(&mut process).await;
                    return Err(e.context(format!(
                        "while waiting for HARNESS_PORT from '{}' cwd='{}' args='--harness {port} {}'",
                        binary.display(),
                        work_dir.display(),
                        extra_args.join(" ")
                    )));
                }
            }
        } else {
            port
        };

        let mut instance = Self {
            process,
            client: None,
            port: actual_port,
            game_binary: binary,
            work_dir,
            config: config.clone(),
            log_tap,
        };

        if let Err(e) = instance.connect_with_retry().await {
            // Same reasoning as the handshake path above: the game booted but
            // never became reachable — reap it before surfacing the error.
            instance.kill_now().await;
            return Err(e);
        }
        Ok(instance)
    }

    /// Kill this instance's game process (and, on Windows, its whole process
    /// tree) and reap it.  Safe to call on an already-exited instance.
    pub async fn kill_now(&mut self) {
        kill_process_tree(&mut self.process).await;
    }

    /// Retry connecting to the harness TCP port using settings from [`ClientConfig`].
    async fn connect_with_retry(&mut self) -> Result<()> {
        let addr = format!("127.0.0.1:{}", self.port);
        let max_retries = self.config.max_retries;
        let delay = self.config.retry_delay;

        for attempt in 1..=max_retries {
            // Check if the process has already crashed before retrying
            if let Some(status) = self.process.try_wait()? {
                anyhow::bail!(
                    "game process '{}' exited with {} before harness became reachable on {addr}",
                    self.game_binary.display(),
                    status
                );
            }

            match HarnessClient::connect_with_config(&addr, self.config.clone()).await {
                Ok(client) => {
                    tracing::debug!("Connected to harness on attempt {attempt}");
                    self.client = Some(client);
                    return Ok(());
                }
                Err(_) if attempt < max_retries => {
                    tracing::trace!(
                        "Harness not ready on attempt {attempt}/{max_retries}, retrying in {delay:?}"
                    );
                    tokio::time::sleep(delay).await;
                }
                Err(e) => {
                    return Err(e).context(format!(
                        "failed to connect to harness at {addr} after {max_retries} attempts \
                         (game binary: {})",
                        self.game_binary.display()
                    ));
                }
            }
        }
        unreachable!()
    }

    /// Get a mutable reference to the harness client.
    pub fn client(&mut self) -> &mut HarnessClient {
        self.client.as_mut().expect("harness client not connected")
    }

    /// Take ownership of the harness client (disconnects it from this instance).
    pub fn take_client(&mut self) -> HarnessClient {
        self.client.take().expect("harness client not connected")
    }

    /// Restore a previously detached harness client.
    pub fn restore_client(&mut self, client: HarnessClient) {
        self.client = Some(client);
    }

    /// Ask a still-attached harness client to end the test.
    pub async fn request_end_test(&mut self) {
        if let Some(client) = self.client.as_mut() {
            let _ = client.exec("triEndTest").await;
        }
    }

    /// Wait for the "ready" event from the harness (main menu loaded).
    pub async fn wait_ready(&mut self, timeout: Duration) -> Result<Event> {
        self.client().wait_for_ready(timeout).await
    }

    /// Wait for the game process to exit and return the exit code.
    ///
    /// The shutdown budget is decoupled from the per-test assertion
    /// timeout: even fast tests (`timeout = 30` in their TOML) get
    /// at least [`EXIT_GRACE`] for the engine to tear down GL,
    /// flush audio, save config, etc.  Under parallel load (3
    /// games sharing a GPU + audio device) shutdown can take
    /// 30+ seconds even for a test whose body ran in 5 s.
    pub async fn wait_exit(&mut self, timeout: Duration) -> Result<i32> {
        const EXIT_GRACE: Duration = Duration::from_secs(60);
        let effective = timeout.max(EXIT_GRACE);
        let pid = self.process.id();
        match tokio::time::timeout(effective, self.process.wait()).await {
            Ok(Ok(status)) => {
                if let Some(pid) = pid {
                    unregister_pid(pid);
                }
                Ok(status.code().unwrap_or(-1))
            }
            Ok(Err(e)) => Err(e.into()),
            Err(_) => {
                tracing::warn!("Game didn't exit within timeout, killing");
                kill_process_tree(&mut self.process).await;
                anyhow::bail!("game process didn't exit within {effective:?}")
            }
        }
    }

    /// Aggressive shutdown after an assertion failure — sends a
    /// graceful `triEndTest` with a short deadline, then force-kills
    /// the game if it doesn't go quietly.  Avoids the 60-second
    /// `EXIT_GRACE` of `wait_exit`, which is sized for clean test
    /// completions under GPU/audio contention — on a known-failing
    /// test, waiting that long is pure latency, and parallel runners
    /// stack the latency to ~16 minutes for 14 failures.  Caller has
    /// already established the test failed; this just reclaims the
    /// slot.
    pub async fn kill_after_failure(&mut self) {
        // Try graceful first (5s budget) — many failures still leave
        // a healthy harness loop that can respond to triEndTest, and
        // a clean exit lets the engine save audio.cfg / etc.
        const GRACEFUL_DEADLINE: Duration = Duration::from_secs(5);
        if let Some(client) = self.client.as_mut() {
            let _ = tokio::time::timeout(GRACEFUL_DEADLINE, client.exec("triEndTest")).await;
        }
        let pid = self.process.id();
        if let Ok(Ok(_)) = tokio::time::timeout(GRACEFUL_DEADLINE, self.process.wait()).await {
            if let Some(pid) = pid {
                unregister_pid(pid);
            }
            return;
        }
        // Wedged — kill the whole tree and wait synchronously so the OS
        // has actually reaped the process before we return.
        tracing::warn!("kill_after_failure: forcing termination");
        kill_process_tree(&mut self.process).await;
    }

    /// Get the port this instance is running on.
    pub const fn port(&self) -> u16 {
        self.port
    }

    /// Get the path to the game binary.
    pub fn binary_path(&self) -> &Path {
        &self.game_binary
    }

    /// Get the working directory used by this game instance.
    pub fn work_dir(&self) -> &Path {
        &self.work_dir
    }
}

// Killing is NOT done in Drop — the Command was built with
// `.kill_on_drop(true)` (see spawn_with_env) as a last-resort backstop,
// and every deliberate teardown path goes through `kill_process_tree`
// (awaited kill + reap, tree-wide on Windows).  An explicit Drop calling
// `start_kill()` here would race with kill_on_drop's std-lib Drop and
// could TerminateProcess on a recycled PID if the child has already
// exited.  The Drop below only maintains the Ctrl-C PID registry.
impl Drop for GameInstance {
    fn drop(&mut self) {
        // If the process was reaped, id() is None and the kill path already
        // unregistered it; otherwise drop the registry entry so a later
        // Ctrl-C cannot taskkill a recycled PID after kill_on_drop fires.
        if let Some(pid) = self.process.id() {
            unregister_pid(pid);
        }
    }
}

/// Find the game binary in a game directory.
///
/// When `named` is Some, looks for that specific binary name first.
/// When `None`, falls back to the default game-binary candidates.
pub fn find_game_binary(game_dir: &Path, named: Option<&str>) -> Result<PathBuf> {
    let ext = if cfg!(windows) { ".exe" } else { "" };
    let with_ext = |n: &str| {
        if n.ends_with(ext) {
            n.to_string()
        } else {
            format!("{n}{ext}")
        }
    };

    // Search game_dir and its bin/ first, then every sibling dir (and their bin/),
    // for `name`. The sibling sweep lets a stale or renamed dist dir still resolve
    // — a game_dir of dist/win-x64-clang-rwdi finds the binary in the actual
    // dist/x64-win-rwdi sitting next to it.
    let locate = |name: &str| -> Option<PathBuf> {
        let mut search = vec![game_dir.join(name), game_dir.join("bin").join(name)];
        if let Some(parent) = game_dir.parent() {
            if let Ok(entries) = std::fs::read_dir(parent) {
                for e in entries.flatten() {
                    let p = e.path();
                    if p.is_dir() && p != game_dir {
                        search.push(p.join(name));
                        search.push(p.join("bin").join(name));
                    }
                }
            }
        }
        search.into_iter().find(|p| p.exists())
    };

    // An explicit --binary takes priority (it may live in a sibling app dir, e.g.
    // PoseidonServer in apps/Server while game_dir points at apps/Game).
    if let Some(n) = named {
        // Upstream fixtures retain the old game target name. This fork ships
        // one canonical client; prefer its local binary over a stale legacy
        // install or sibling checkout when those fixtures run on Windows.
        #[cfg(windows)]
        if matches!(n, "PoseidonGame" | "PoseidonGame.exe") {
            let current = game_dir.join("OpenPoseidon.exe");
            if current.is_file() {
                return Ok(current);
            }
        }
        if let Some(found) = locate(&with_ext(n)) {
            return Ok(found);
        }
    }

    let defaults: &[&str] = if cfg!(windows) {
        &["OpenPoseidon.exe", "PoseidonGame.exe", "OFPR.exe"]
    } else {
        &["PoseidonGame", "OFPR"]
    };
    for name in defaults {
        if let Some(found) = locate(&with_ext(name)) {
            return Ok(found);
        }
    }

    anyhow::bail!(
        "no game binary found in '{}' — looked for {:?} (also checked bin/ + sibling dirs)",
        game_dir.display(),
        defaults
    )
}

/// Read game stdout looking for `HARNESS_PORT=N` announced by the game after binding.
/// Remaining stdout lines are saved to `log_file` (if provided) so game logs are not lost.
async fn read_harness_port(
    process: &mut Child,
    log_file: Option<std::fs::File>,
    log_tap: LogTap,
    timeout: Duration,
) -> Result<u16> {
    let stdout = process
        .stdout
        .take()
        .context("stdout not piped for harness port discovery")?;
    let mut lines = tokio::io::BufReader::new(stdout).lines();
    let deadline = tokio::time::Instant::now() + timeout;
    let mut early_stdout = Vec::new();
    loop {
        let line = tokio::time::timeout_at(deadline, lines.next_line())
            .await
            .map_err(|_| anyhow::anyhow!("timeout waiting for HARNESS_PORT announcement"))?
            .context("I/O error reading game stdout")?;
        match line {
            None => {
                let excerpt = if early_stdout.is_empty() {
                    "<no stdout before exit>".to_string()
                } else {
                    early_stdout.join("\n")
                };
                anyhow::bail!(
                    "game closed stdout before announcing HARNESS_PORT; early stdout:\n{excerpt}"
                );
            }
            Some(line) => {
                if let Some(rest) = line.trim().strip_prefix("HARNESS_PORT=") {
                    if let Ok(port) = rest.parse::<u16>() {
                        // Drain remaining stdout to log file (or discard) so the pipe doesn't block
                        tokio::spawn(async move {
                            let mut writer = log_file.map(tokio::fs::File::from_std);
                            while let Ok(Some(l)) = lines.next_line().await {
                                if let Some(w) = writer.as_mut() {
                                    use tokio::io::AsyncWriteExt;
                                    let _ = w.write_all(format!("{l}\n").as_bytes()).await;
                                }
                                if let Ok(mut tap) = log_tap.lock() {
                                    tap.push(l);
                                }
                            }
                        });
                        return Ok(port);
                    }
                }
                if early_stdout.len() < 80 {
                    early_stdout.push(line);
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use tempfile::TempDir;

    #[test]
    fn log_tap_preserves_order_and_refuses_missing_or_truncated_windows() {
        let mut tap = LogTapState::default();
        tap.push("before".to_owned());
        let mark = tap.lines.len();
        tap.push("first".to_owned());
        tap.push("second".to_owned());
        assert_eq!(tap.since(mark).unwrap(), vec!["first", "second"]);
        assert!(tap.since(mark + 3).is_err());
        tap.bytes = 16 * 1024 * 1024;
        tap.push("cannot retain".to_owned());
        assert!(tap.overflowed);
        assert!(tap.since(mark).is_err());
    }

    #[test]
    fn find_binary_not_found() {
        let dir = TempDir::new().unwrap();
        // Nested dir so the sibling sweep only sees the (empty) TempDir, not
        // whatever else happens to be in the system temp root.
        let game = dir.path().join("game");
        std::fs::create_dir(&game).unwrap();
        let result = find_game_binary(&game, None);
        assert!(result.is_err());
        let msg = result.unwrap_err().to_string();
        assert!(msg.contains("no game binary found"));
    }

    #[test]
    fn find_binary_in_sibling_dir() {
        // A game_dir whose own binary is missing but a *sibling* dir has it (the
        // stale/renamed dist case, e.g. win-x64-clang-rwdi vs x64-win-rwdi) must
        // still resolve via the sibling sweep. Before the fix this bailed.
        let root = TempDir::new().unwrap();
        let wrong = root.path().join("win-x64-clang-rwdi");
        std::fs::create_dir(&wrong).unwrap();
        let right = root.path().join("x64-win-rwdi");
        std::fs::create_dir(&right).unwrap();
        let binary = right.join(if cfg!(windows) {
            "PoseidonGame.exe"
        } else {
            "PoseidonGame"
        });
        std::fs::write(&binary, "").unwrap();

        let result = find_game_binary(&wrong, None).unwrap();
        assert_eq!(result, binary);
    }

    #[test]
    fn find_binary_direct() {
        let dir = TempDir::new().unwrap();
        let binary = dir.path().join(if cfg!(windows) {
            "PoseidonGame.exe"
        } else {
            "PoseidonGame"
        });
        std::fs::write(&binary, "").unwrap();
        let result = find_game_binary(dir.path(), None).unwrap();
        assert_eq!(result, binary);
    }

    #[cfg(windows)]
    #[test]
    fn upstream_client_fixture_uses_current_fork_binary_over_stale_client() {
        let dir = TempDir::new().unwrap();
        let current = dir.path().join("OpenPoseidon.exe");
        std::fs::write(&current, "current fork").unwrap();
        std::fs::write(dir.path().join("PoseidonGame.exe"), "stale install").unwrap();
        let server = dir.path().join("PoseidonServer.exe");
        std::fs::write(&server, "server").unwrap();
        for name in [None, Some("PoseidonGame"), Some("PoseidonGame.exe")] {
            assert_eq!(find_game_binary(dir.path(), name).unwrap(), current);
        }
        assert_eq!(
            find_game_binary(dir.path(), Some("PoseidonServer")).unwrap(),
            server
        );
    }

    #[test]
    fn find_binary_in_bin_subdir() {
        let dir = TempDir::new().unwrap();
        let bin_dir = dir.path().join("bin");
        std::fs::create_dir(&bin_dir).unwrap();
        let binary = bin_dir.join(if cfg!(windows) {
            "PoseidonGame.exe"
        } else {
            "PoseidonGame"
        });
        std::fs::write(&binary, "").unwrap();
        let result = find_game_binary(dir.path(), None).unwrap();
        assert_eq!(result, binary);
    }

    /// A dummy child that would outlive the test by a minute unless killed —
    /// stands in for a game that never announces `HARNESS_PORT`.
    fn long_running_child() -> Child {
        let mut cmd = if cfg!(windows) {
            let mut c = Command::new("cmd");
            c.args(["/C", "ping -n 60 127.0.0.1 > NUL"]);
            c
        } else {
            let mut c = Command::new("sleep");
            c.arg("60");
            c
        };
        cmd.stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
            .expect("failed to spawn dummy child")
    }

    #[tokio::test]
    async fn kill_process_tree_terminates_live_child() {
        let mut child = long_running_child();
        let pid = child.id().expect("live child has a pid");

        kill_process_tree(&mut child).await;

        // The child must be dead AND reaped: try_wait reports an exit status
        // rather than "still running".  This is the exact property the
        // handshake-timeout path needs — a dropped Child does not give it.
        assert!(
            matches!(child.try_wait(), Ok(Some(_))),
            "child not reaped after kill_process_tree"
        );

        // Ask the OS directly that the PID is gone (Windows: tasklist filter
        // returns no matching row).  On unix, try_wait proving the reap is
        // sufficient — a `kill -0` probe would race PID recycling.
        #[cfg(windows)]
        {
            let out = std::process::Command::new("tasklist")
                .args(["/FI", &format!("PID eq {pid}"), "/NH"])
                .output()
                .expect("tasklist failed to run");
            let text = String::from_utf8_lossy(&out.stdout);
            assert!(
                !text.contains(&pid.to_string()),
                "pid {pid} still alive after kill_process_tree: {text}"
            );
        }
        let _ = pid;
    }

    #[tokio::test]
    async fn kill_process_tree_is_safe_on_already_exited_child() {
        let mut cmd = if cfg!(windows) {
            let mut c = Command::new("cmd");
            c.args(["/C", "exit 0"]);
            c
        } else {
            Command::new("true")
        };
        let mut child = cmd
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
            .expect("failed to spawn dummy child");
        let _ = child.wait().await;

        // Must neither hang nor taskkill a recycled PID; just a no-op.
        kill_process_tree(&mut child).await;
        assert!(matches!(child.try_wait(), Ok(Some(_))));
    }

    #[tokio::test]
    async fn kill_all_registered_games_kills_registered_pid() {
        let mut child = long_running_child();
        let pid = child.id().expect("live child has a pid");
        register_pid(pid);

        // Simulates the Ctrl-C handler: synchronous, no reliance on the
        // child future being polled.
        kill_all_registered_games();

        let waited = tokio::time::timeout(Duration::from_secs(10), child.wait()).await;
        assert!(
            waited.is_ok(),
            "registered child survived kill_all_registered_games"
        );
    }

    #[test]
    fn global_config_derives_client_config() {
        let cfg = crate::config::TridentConfig::new(5);
        let cc = cfg.client_config();
        assert_eq!(cc.max_retries, 10); // 5s * 2
        assert_eq!(cc.retry_delay, Duration::from_millis(500));
        assert_eq!(cc.command_timeout, Duration::from_secs(5));
    }
}
