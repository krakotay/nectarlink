// SPDX-License-Identifier: GPL-3.0-or-later
//! One completed picture waiting for its consumer, never a replay queue.

use std::{
    sync::{Condvar, Mutex},
    time::Duration,
};

struct State<T> {
    frame: Option<T>,
    closed: bool,
    replaced: u32,
}

pub(super) struct Latest<T> {
    state: Mutex<State<T>>,
    ready: Condvar,
}

impl<T> Latest<T> {
    pub(super) fn new() -> Self {
        Self { state: Mutex::new(State { frame: None, closed: false, replaced: 0 }), ready: Condvar::new() }
    }

    pub(super) fn publish(&self, frame: T) {
        let mut s = self.state.lock().unwrap_or_else(|e| e.into_inner());
        if s.closed {
            return;
        }
        if s.frame.replace(frame).is_some() {
            s.replaced = s.replaced.saturating_add(1);
        }
        self.ready.notify_one();
    }

    pub(super) fn take(&self, timeout: Duration) -> Option<T> {
        let s = self.state.lock().unwrap_or_else(|e| e.into_inner());
        let (mut s, _) = self
            .ready
            .wait_timeout_while(s, timeout, |s| s.frame.is_none() && !s.closed)
            .unwrap_or_else(|e| e.into_inner());
        s.frame.take()
    }

    pub(super) fn clear(&self) {
        self.state.lock().unwrap_or_else(|e| e.into_inner()).frame = None;
    }

    pub(super) fn replaced(&self) -> u32 {
        std::mem::take(&mut self.state.lock().unwrap_or_else(|e| e.into_inner()).replaced)
    }

    pub(super) fn is_closed(&self) -> bool {
        self.state.lock().unwrap_or_else(|e| e.into_inner()).closed
    }

    pub(super) fn close(&self) {
        let mut s = self.state.lock().unwrap_or_else(|e| e.into_inner());
        s.closed = true;
        s.frame = None;
        self.ready.notify_all();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn a_busy_renderer_receives_only_the_newest_completed_frame() {
        let q = Latest::new();
        for frame in 0..100 {
            q.publish(frame);
        }
        assert_eq!(q.take(Duration::ZERO), Some(99));
        assert_eq!(q.replaced(), 99);
        assert_eq!(q.replaced(), 0);
        assert_eq!(q.take(Duration::ZERO), None);
    }
    #[test]
    fn reset_and_shutdown_do_not_replay_old_frames() {
        let q = Latest::new();
        q.publish(1);
        q.clear();
        assert_eq!(q.take(Duration::ZERO), None);
        q.publish(2);
        q.close();
        q.publish(3);
        assert!(q.is_closed());
        assert_eq!(q.take(Duration::ZERO), None);
    }
}
