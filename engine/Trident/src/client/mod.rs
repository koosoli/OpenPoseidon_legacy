//! TCP client for communicating with the game harness port.

mod connection;
mod instance;
pub mod port_alloc;

pub use connection::{ClientConfig, HarnessClient};
pub use instance::{find_game_binary, kill_all_registered_games, kill_process_tree, GameInstance};
