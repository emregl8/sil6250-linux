// SPDX-License-Identifier: GPL-2.0

use std::io;
use std::sync::Arc;
use std::thread::sleep;
use std::time::{Duration, Instant};

const CHECK_INTERVAL: Duration = Duration::from_millis(50);

#[derive(Clone, Default)]
pub struct OperationControl {
    deadline: Option<Instant>,
    cancelled: Option<Arc<dyn Fn() -> bool + Send + Sync>>,
}

impl OperationControl {
    pub fn new(timeout: Duration, cancelled: impl Fn() -> bool + Send + Sync + 'static) -> Self {
        Self {
            deadline: Some(
                Instant::now()
                    .checked_add(timeout)
                    .unwrap_or_else(Instant::now),
            ),
            cancelled: Some(Arc::new(cancelled)),
        }
    }

    pub fn check(&self) -> io::Result<()> {
        if self.cancelled.as_ref().is_some_and(|cancelled| cancelled()) {
            return Err(io::Error::from(io::ErrorKind::Interrupted));
        }
        if self
            .deadline
            .is_some_and(|deadline| Instant::now() >= deadline)
        {
            return Err(io::Error::from(io::ErrorKind::TimedOut));
        }
        Ok(())
    }

    pub fn sleep(&self, duration: Duration) -> io::Result<()> {
        let end = Instant::now() + duration;
        loop {
            self.check()?;
            let now = Instant::now();
            if now >= end {
                return Ok(());
            }
            let remaining = self
                .deadline
                .map_or(end, |deadline| deadline.min(end))
                .saturating_duration_since(now);
            sleep(remaining.min(CHECK_INTERVAL));
        }
    }

    pub fn wait_irq(
        &self,
        timeout: Duration,
        mut wait: impl FnMut(u32) -> io::Result<()>,
    ) -> io::Result<()> {
        let end = Instant::now() + timeout;
        loop {
            self.check()?;
            let now = Instant::now();
            if now >= end {
                return Err(io::Error::from_raw_os_error(nix::libc::ETIMEDOUT));
            }
            let remaining = self
                .deadline
                .map_or(end, |deadline| deadline.min(end))
                .saturating_duration_since(now);
            let milliseconds = remaining.min(CHECK_INTERVAL).as_millis().max(1) as u32;
            match wait(milliseconds) {
                Ok(()) => return self.check(),
                Err(error) if error.kind() == io::ErrorKind::TimedOut => {}
                Err(error) => return Err(error),
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicBool, Ordering};

    #[test]
    fn expired_budget_never_starts_io() {
        let control = OperationControl::new(Duration::ZERO, || false);
        let error = control
            .wait_irq(Duration::from_secs(1), |_| panic!("I/O after expiry"))
            .unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::TimedOut);
    }

    #[test]
    fn cancellation_interrupts_irq_retry() {
        let cancelled = Arc::new(AtomicBool::new(false));
        let flag = cancelled.clone();
        let control =
            OperationControl::new(Duration::from_secs(1), move || flag.load(Ordering::Relaxed));
        let mut calls = 0;
        let error = control
            .wait_irq(Duration::from_secs(1), |timeout| {
                assert!(timeout <= 50);
                calls += 1;
                cancelled.store(true, Ordering::Relaxed);
                Err(io::Error::from_raw_os_error(nix::libc::ETIMEDOUT))
            })
            .unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::Interrupted);
        assert_eq!(calls, 1);
    }

    #[test]
    fn irq_errors_are_not_retried() {
        let control = OperationControl::default();
        let error = control
            .wait_irq(Duration::from_secs(1), |_| {
                Err(io::Error::from_raw_os_error(nix::libc::EIO))
            })
            .unwrap_err();
        assert_eq!(error.raw_os_error(), Some(nix::libc::EIO));
    }

    #[test]
    fn cancellation_after_irq_never_reports_success() {
        let cancelled = Arc::new(AtomicBool::new(false));
        let flag = cancelled.clone();
        let control =
            OperationControl::new(Duration::from_secs(1), move || flag.load(Ordering::Relaxed));
        let error = control
            .wait_irq(Duration::from_secs(1), |_| {
                cancelled.store(true, Ordering::Relaxed);
                Ok(())
            })
            .unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::Interrupted);
    }

    #[test]
    fn sleep_obeys_operation_deadline() {
        let control = OperationControl::new(Duration::from_millis(10), || false);
        assert_eq!(
            control.sleep(Duration::from_secs(1)).unwrap_err().kind(),
            io::ErrorKind::TimedOut
        );
    }
}
