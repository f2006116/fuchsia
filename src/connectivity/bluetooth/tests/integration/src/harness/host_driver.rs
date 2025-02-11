// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    failure::{Error, ResultExt},
    fidl_fuchsia_bluetooth_control::{AdapterInfo, AdapterState, RemoteDevice},
    fidl_fuchsia_bluetooth_host::{HostEvent, HostProxy},
    fuchsia_async::{self as fasync, TimeoutExt},
    fuchsia_bluetooth::{
        constants::HOST_DEVICE_DIR, device_watcher::DeviceWatcher, error::Error as BtError,
        expectation::Predicate, hci_emulator::Emulator, host, util::clone_host_state,
    },
    fuchsia_zircon::{Duration, DurationNum},
    futures::{future, task, Future, FutureExt, Poll, TryFutureExt, TryStreamExt},
    parking_lot::{MappedRwLockReadGuard, RwLock, RwLockReadGuard},
    slab::Slab,
    std::{borrow::Borrow, collections::HashMap, path::PathBuf, pin::Pin, sync::Arc},
};

use crate::harness::TestHarness;

const TIMEOUT_SECONDS: i64 = 10; // in seconds

fn timeout_duration() -> Duration {
    TIMEOUT_SECONDS.seconds()
}

#[derive(Clone)]
pub struct HostDriverHarness(Arc<RwLock<HostDriverHarnessInner>>);

struct HostDriverHarnessInner {
    // Fake bt-hci device.
    hci_emulator: Option<Emulator>,

    // Access to the bt-host device under test.
    host_path: PathBuf,
    pub host_proxy: HostProxy,

    // Current bt-host driver state.
    host_info: AdapterInfo,

    // All known remote devices, indexed by their identifiers.
    remote_devices: HashMap<String, RemoteDevice>,

    // Tasks that are interested in being woken up when the host drirver state changes.
    host_state_tasks: Slab<task::Waker>,
}

impl HostDriverHarness {
    // Returns a Future that resolves when the bt-host transitions to a state that matches the
    // predicate `target`.
    //
    // For example, if `target` is
    //
    //   expectation::host_driver::discoverable(true)
    //     .and(expectation::host_driver::discovering(true));
    //
    // then the Future will resolve when the host driver becomes discoverable AND discovering. Other
    // fields will be ignored.
    //
    // If the host driver is already in the requested state, then the Future will resolve the first
    // time it gets polled.
    pub async fn expect(&self, target: Predicate<AdapterState>) -> Result<AdapterState, Error> {
        let err_msg = format!("timed out waiting for host driver state (expected: {:?})", target);
        await!(HostDriverStateFuture::new(self.clone(), target)
            .on_timeout(timeout_duration().after_now(), move || Err(BtError::new(&err_msg).into())))
    }

    // Returns a Future that resolves when the state of a particular RemoteDevice matches
    // `target`. If `id` is None, then the Future will resolve when any device matches
    // `target`. Otherwise, the Future will resolve when the state of the requested device
    // changes.
    pub async fn expect_peer(
        &self,
        id: Option<String>,
        target: Predicate<RemoteDevice>,
    ) -> Result<(), Error> {
        let err_msg = format!("timed out waiting for remote device state (expected: {:?})", target);
        await!(RemoteDeviceStateFuture::new(self.clone(), id, Some(target))
            .on_timeout(timeout_duration().after_now(), move || Err(BtError::new(&err_msg).into())))
    }

    // Returns a future that resolves when a peer matching `id` is not present on the host.
    pub async fn expect_no_peer(&self, id: String) -> Result<(), Error> {
        let err_msg = format!("timed out waiting for peer to be removed");
        await!(RemoteDeviceStateFuture::new(self.clone(), Some(id), None)
            .on_timeout(timeout_duration().after_now(), move || Err(BtError::new(&err_msg).into())))
    }

    pub fn host_proxy(&self) -> MappedRwLockReadGuard<HostProxy> {
        RwLockReadGuard::map(self.0.read(), |host| &host.host_proxy)
    }
}

impl HostDriverHarnessInner {
    fn new(
        hci: Emulator,
        host_path: PathBuf,
        host: HostProxy,
        info: AdapterInfo,
    ) -> HostDriverHarness {
        HostDriverHarness(Arc::new(RwLock::new(HostDriverHarnessInner {
            hci_emulator: Some(hci),
            host_path: host_path,
            host_proxy: host,
            host_info: info,
            remote_devices: HashMap::new(),
            host_state_tasks: Slab::new(),
        })))
    }

    // Returns a Future that handles Host interface events.
    fn events_future(test_state: HostDriverHarness) -> impl Future<Output = Result<(), Error>> {
        let stream = test_state.0.read().host_proxy.take_event_stream();
        stream
            .try_for_each(move |evt| {
                match evt {
                    HostEvent::OnAdapterStateChanged { state } => {
                        test_state.0.write().handle_driver_state_changed(state);
                    }
                    HostEvent::OnDeviceUpdated { device } => {
                        test_state.0.write().handle_device_updated(device);
                    }
                    HostEvent::OnDeviceRemoved { identifier } => {
                        test_state.0.write().handle_device_removed(identifier);
                    }
                    // TODO(armansito): handle other events
                    evt => {
                        eprintln!("Unhandled event: {:?}", evt);
                    }
                }
                future::ready(Ok(()))
            })
            .err_into()
    }

    fn close_fake_hci(&mut self) {
        self.hci_emulator = None;
    }

    fn store_task(&mut self, task: task::Waker) -> usize {
        self.host_state_tasks.insert(task)
    }

    fn remove_task(&mut self, key: usize) {
        if self.host_state_tasks.contains(key) {
            self.host_state_tasks.remove(key);
        }
    }

    fn find_device_by_address(&self, address: &str) -> Result<&RemoteDevice, Error> {
        self.remote_devices
            .values()
            .find(|dev| dev.address == address)
            .ok_or(BtError::new(&format!("device with address '{}' not found", address)).into())
    }

    // Handle the OnAdapterStateChanged event.
    fn handle_driver_state_changed(&mut self, state_change: AdapterState) {
        let base = match self.host_info.state {
            Some(ref state) => clone_host_state(state.borrow()),
            None => AdapterState {
                local_name: None,
                discoverable: None,
                discovering: None,
                local_service_uuids: None,
            },
        };
        let new_state = apply_delta(base, state_change);
        self.host_info.state = Some(Box::new(new_state));
        self.notify_host_state_changed();
    }

    // Handle the OnDeviceUpdated event
    fn handle_device_updated(&mut self, device: RemoteDevice) {
        self.remote_devices.insert(device.identifier.clone(), device);
        self.notify_host_state_changed();
    }

    // Handle the OnDeviceRemoved event
    fn handle_device_removed(&mut self, id: String) {
        self.remote_devices.remove(&id);
        self.notify_host_state_changed();
    }

    fn notify_host_state_changed(&mut self) {
        for task in &self.host_state_tasks {
            task.1.wake_by_ref();
        }
        self.host_state_tasks.clear()
    }
}

// Applies `delta` to `base`.
fn apply_delta(base: AdapterState, delta: AdapterState) -> AdapterState {
    AdapterState {
        local_name: delta.local_name.or(base.local_name),
        discoverable: delta.discoverable.or(base.discoverable),
        discovering: delta.discovering.or(base.discovering),
        local_service_uuids: delta.local_service_uuids.or(base.local_service_uuids),
    }
}

// A future that resolves when the state of a remote device changes to a target value.
struct RemoteDeviceStateFuture {
    inner: StateUpdateFutureInner,

    // The identifier of the desired RemoteDevice. If None, then this Future can resolve with any
    // device.
    target_dev_id: Option<String>,

    // The expected state a RemoteDevice is expected to reach. If the state is
    // None then the Future will resolve when the device gets removed. If both `target_dev_state`
    // and `target_dev_id` are None, then the Future will resolve when any device gets removed.
    target_dev_state: Option<Predicate<RemoteDevice>>,
}

impl RemoteDeviceStateFuture {
    fn new(
        test: HostDriverHarness,
        target_id: Option<String>,
        target_state: Option<Predicate<RemoteDevice>>,
    ) -> Self {
        RemoteDeviceStateFuture {
            inner: StateUpdateFutureInner::new(test),
            target_dev_id: target_id,
            target_dev_state: target_state,
        }
    }

    fn look_for_match(&self) -> bool {
        match &self.target_dev_id {
            None => self.match_any_device(&self.target_dev_state),
            Some(id) => self.match_device_by_id(&id, &self.target_dev_state),
        }
    }

    fn match_any_device(&self, target: &Option<Predicate<RemoteDevice>>) -> bool {
        target.as_ref().map_or(false, |target| {
            self.inner
                .state
                .0
                .read()
                .remote_devices
                .values()
                .find(|dev| target.satisfied(dev))
                .is_some()
        })
    }

    fn match_device_by_id(&self, id: &str, target: &Option<Predicate<RemoteDevice>>) -> bool {
        match (self.inner.state.0.read().remote_devices.get(id), target) {
            (None, None) => true,
            (Some(dev), Some(target)) => target.satisfied(dev),
            _ => false,
        }
    }
}
struct StateUpdateFutureInner {
    state: HostDriverHarness,
    waker_key: Option<usize>,
}

impl StateUpdateFutureInner {
    fn new(state: HostDriverHarness) -> StateUpdateFutureInner {
        StateUpdateFutureInner { state, waker_key: None }
    }

    fn clear_waker(&mut self) {
        if let Some(key) = self.waker_key {
            self.state.0.write().remove_task(key);
            self.waker_key = None;
        }
    }

    fn store_task(&mut self, w: &task::Waker) {
        let key = self.state.0.write().store_task(w.clone());
        self.waker_key = Some(key);
    }
}

// A future that resolves when the bt-host's AdapterState changes to a certain target value.
struct HostDriverStateFuture {
    inner: StateUpdateFutureInner,

    // The expected host driver state.
    target_host_state: Predicate<AdapterState>,
}

impl HostDriverStateFuture {
    fn new(test: HostDriverHarness, target_state: Predicate<AdapterState>) -> Self {
        HostDriverStateFuture {
            inner: StateUpdateFutureInner::new(test),
            target_host_state: target_state,
        }
    }
}

impl std::marker::Unpin for HostDriverStateFuture {}

#[must_use = "futures do nothing unless polled"]
impl Future for HostDriverStateFuture {
    type Output = Result<AdapterState, Error>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut task::Context<'_>) -> Poll<Self::Output> {
        self.inner.clear_waker();
        if let Some(state) = &self.inner.state.0.read().host_info.state {
            if self.target_host_state.satisfied(state.borrow()) {
                return Poll::Ready(Ok(clone_host_state(&state.borrow())));
            }
        };
        self.inner.store_task(cx.waker());
        Poll::Pending
    }
}

impl std::marker::Unpin for RemoteDeviceStateFuture {}

#[must_use = "futures do nothing unless polled"]
impl Future for RemoteDeviceStateFuture {
    type Output = Result<(), Error>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut task::Context<'_>) -> Poll<Self::Output> {
        self.inner.clear_waker();
        if self.look_for_match() {
            Poll::Ready(Ok(()))
        } else {
            self.inner.store_task(cx.waker());
            Poll::Pending
        }
    }
}

// Creates a fake bt-hci device and returns the corresponding bt-host device once it gets created.
async fn setup_emulated_host_test() -> Result<HostDriverHarness, Error> {
    let emulator = await!(Emulator::create("bt-integration-test-host"))?;
    let host_dev = await!(emulator.publish_and_wait_for_host(Emulator::default_settings()))?;

    // Open a Host FIDL interface channel to the bt-host device.
    let channel = host::open_host_channel(host_dev.file())?;
    let host = HostProxy::new(fasync::Channel::from_channel(channel.into())?);
    let info = await!(host.get_info())?;

    Ok(HostDriverHarnessInner::new(emulator, host_dev.path().to_path_buf(), host, info))
}

async fn run_host_test_async<F, Fut>(test_func: F) -> Result<(), Error>
where
    F: FnOnce(HostDriverHarness) -> Fut,
    Fut: Future<Output = Result<(), Error>>,
{
    let host_test = await!(setup_emulated_host_test())?;

    // Start processing events in a background task.
    fasync::spawn(HostDriverHarnessInner::events_future(host_test.clone()).map(|_| ()));

    // Run the test and obtain the test result.
    let result = await!(test_func(host_test.clone()));

    // Shut down the fake bt-hci device and make sure the bt-host device gets removed.
    let mut watcher = DeviceWatcher::new(HOST_DEVICE_DIR, timeout_duration())?;
    host_test.0.write().close_fake_hci();

    let host_path = &host_test.0.read().host_path;
    await!(watcher.watch_removed(host_path))?;
    result
}

impl TestHarness for HostDriverHarness {
    fn run_with_harness<F, Fut>(test_func: F) -> Result<(), Error>
    where
        F: FnOnce(Self) -> Fut,
        Fut: Future<Output = Result<(), Error>>,
    {
        let mut executor = fasync::Executor::new().context("error creating event loop")?;
        executor.run_singlethreaded(run_host_test_async(test_func))
    }
}

pub fn expect_eq<T>(expected: &T, actual: &T) -> Result<(), Error>
where
    T: std::fmt::Debug + std::cmp::PartialEq,
{
    if *expected == *actual {
        Ok(())
    } else {
        Err(BtError::new(&format!("failed - expected '{:#?}', found: '{:#?}'", expected, actual))
            .into())
    }
}

macro_rules! expect_eq {
    ($expected:expr, $actual:expr) => {
        expect_eq(&$expected, &$actual)
    };
}

macro_rules! expect_true {
    ($condition:expr) => {
        if $condition{
            Ok(())
        } else {
            Err(fuchsia_bluetooth::error::Error::new(&format!(
                "condition is not true: {}",
                stringify!($condition)
            )).into())
        } as Result<(), Error>
    }
}

pub fn expect_remote_device(
    test_state: &HostDriverHarness,
    address: &str,
    expected: &Predicate<RemoteDevice>,
) -> Result<(), Error> {
    expect_true!(expected.satisfied(test_state.0.read().find_device_by_address(address)?))
}
