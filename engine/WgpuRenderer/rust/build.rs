use std::path::Path;
use std::process::Command;

fn git_revision(repo: &Path) -> Option<String> {
    let output = Command::new("git")
        .args(["rev-parse", "--short=12", "HEAD"])
        .current_dir(repo)
        .output()
        .ok()?;
    if output.status.success() {
        Some(String::from_utf8_lossy(&output.stdout).trim().to_owned())
    } else {
        None
    }
}

// The file whose mtime actually moves when HEAD's commit changes.
//
// `.git/HEAD` on a branch is a SYMBOLIC ref -- its contents are the fixed line
// `ref: refs/heads/<branch>` -- so it changes when you switch branches and NOT when
// you commit on one. Watching only that file is why the DLL kept reporting a build
// id from four commits earlier while every capture quoted it as provenance: cargo
// saw no reason to re-run this script, so WGR_BUILD_ID was never restamped.
//
// Returns the ref file HEAD names, or None for a detached HEAD (where `.git/HEAD`
// holds the sha itself and IS the file that moves) and for a ref that lives in
// `.git/packed-refs` rather than as a loose file.
fn head_ref_file(repo: &Path) -> Option<std::path::PathBuf> {
    let head = std::fs::read_to_string(repo.join(".git/HEAD")).ok()?;
    let target = head.trim().strip_prefix("ref:")?.trim();
    let path = repo.join(".git").join(target);
    path.exists().then_some(path)
}

fn main() {
    let manifest =
        std::path::PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").expect("manifest directory"));
    let repo = manifest.ancestors().nth(3).expect("repository root");
    let revision = git_revision(repo).unwrap_or_else(|| "unknown".to_owned());
    let profile = std::env::var("PROFILE").unwrap_or_else(|_| "unknown".to_owned());
    println!("cargo:rustc-env=WGR_BUILD_ID={revision}-{profile}");
    // A checked-out ref moves by updating HEAD or its referenced file. The source
    // build still records `unknown` safely when git metadata is unavailable.
    println!(
        "cargo:rerun-if-changed={}",
        repo.join(".git/HEAD").display()
    );
    // ...and the referenced file, which is the one that moves on a plain commit.
    // `packed-refs` covers the case where the branch has no loose ref file.
    if let Some(reffile) = head_ref_file(repo) {
        println!("cargo:rerun-if-changed={}", reffile.display());
    }
    let packed = repo.join(".git/packed-refs");
    if packed.exists() {
        println!("cargo:rerun-if-changed={}", packed.display());
    }
    println!("cargo:rerun-if-env-changed=PROFILE");
    // Test-only override used to produce a compatible-import ABI mismatch DLL.
    // It is never set by CMake or a normal Cargo invocation.
    println!("cargo:rerun-if-env-changed=WGR_TEST_ABI_VERSION");
}
