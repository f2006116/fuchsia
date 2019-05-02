// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#[cfg(test)]
mod tests;

use crate::{
    common::App,
    configuration::Config,
    protocol::{
        request::{
            Event, InstallSource, Ping, Request, RequestWrapper, UpdateCheck, HEADER_APP_ID,
            HEADER_INTERACTIVITY, HEADER_UPDATER_NAME,
        },
        Cohort, PROTOCOL_V3,
    },
};

use http;
use log::*;
use std::result;

type ProtocolApp = crate::protocol::request::App;

/// Building a request can fail for multiple reasons, this enum consolidates them into a single
/// type that can be used to express those reasons.
#[derive(Debug)]
pub enum Error {
    Json(serde_json::Error),
    Http(http::Error),
}

impl From<serde_json::Error> for Error {
    fn from(e: serde_json::Error) -> Self {
        Error::Json(e)
    }
}

impl From<http::Error> for Error {
    fn from(e: http::Error) -> Self {
        Error::Http(e)
    }
}

/// The builder's own Result type.
pub type Result<T> = result::Result<T, Error>;

/// These are the parameters that describe how the request should be performed.
#[derive(Clone, Debug, PartialEq)]
pub struct RequestParams {
    /// The install source for a request changes a number of properties of the request, including
    /// the HTTP request headers, and influences how Omaha services the request (e.g. throttling)
    pub source: InstallSource,

    /// If true, the request should use any configured proxies.  This allows the bypassing of
    /// proxies if there are difficulties in communicating with the Omaha service.
    pub use_configured_proxies: bool,
}

/// The AppEntry holds the data for the app whose request is currently being constructed.  An app
/// can only have a single cohort, update check, or ping, but may have multiple events.  Note that
/// while this object allows for no update check, no ping, and no events, that doesn't make sense
/// via the protocol.
///
/// This struct has ownership over it's members, so that they may later be moved out when the
/// request itself is built.
struct AppEntry {
    /// The identifying data for the application.
    app: App,

    /// The optionally-present cohort that this application belongs to.
    cohort: Option<Cohort>,

    /// Set to true if an update check should be performed.
    update_check: bool,

    /// A ping, if that's being performed.
    ping: Option<Ping>,

    /// Any events that need to be sent to the Omaha service.
    events: Vec<Event>,
}

impl AppEntry {
    /// Basic constructor for the AppEntry.  All AppEntries MUST have an App and a Cohort,
    /// everything else can be omitted.
    fn new(app: &App, cohort: &Option<Cohort>) -> AppEntry {
        AppEntry {
            app: app.clone(),
            cohort: cohort.clone(),
            update_check: false,
            ping: None,
            events: Vec::new(),
        }
    }
}

/// Conversion method to construct a ProtocolApp from an AppEntry.  This consumes the entry, moving
/// it's members into the generated ProtocolApp.
impl From<AppEntry> for ProtocolApp {
    fn from(entry: AppEntry) -> ProtocolApp {
        if !entry.update_check && entry.events.is_empty() && entry.ping == None {
            warn!(
                "Generated protocol::request for {} has no update check, ping, or events",
                entry.app.id
            );
        }
        ProtocolApp {
            id: entry.app.id,
            version: entry.app.version.to_string(),
            fingerprint: entry.app.fingerprint,
            cohort: entry.cohort,
            update_check: if entry.update_check { Some(UpdateCheck::default()) } else { None },
            events: entry.events,
            ping: entry.ping,
        }
    }
}

/// The RequestBuilder is used to create the protocol requests.  Each request is represented by an
/// instance of protocol::request::Request.
pub struct RequestBuilder<'a> {
    // The static data identifying the updater binary.
    config: &'a Config,

    // The parameters that control how this request is to be made.
    params: RequestParams,

    // The applications to include in this request, with their associated update checks, pings, and
    // events to report.
    app_entries: Vec<AppEntry>,
}

/// The RequestBuilder is a stateful builder for protocol::request::Request objects.  After being
/// instantiated with the base parameters for the current request, it has functions for accumulating
/// an update check, a ping, and multiple events for individual App objects.
///
/// The 'add_*()' functions are all insensitive to order for a given App and it's Cohort.  However,
/// if multiple different App entries are used, then order matters.  The order in the request is
/// the order that the Apps are added to the RequestBuilder.
///
/// Further, the cohort is only captured on the _first_ time a given App is added to the request.
/// If, for some reason, the same App is added twice, but with a different cohort, the latter cohort
/// is ignored.
///
/// The operation being added (update check, ping, or event) is added to the existing App.  The app
/// maintains its existing place in the list of Apps to be added to the request.
impl<'a> RequestBuilder<'a> {
    /// Constructor for creating a new RequestBuilder based on the Updater configuration and the
    /// parameters for the current request.
    pub fn new(config: &'a Config, params: &RequestParams) -> Self {
        RequestBuilder { config, params: params.clone(), app_entries: Vec::new() }
    }

    /// Insert the given app (with its cohort), and run the associated closure on it.  If the app
    /// already exists in the request (by app id), just run the closure on the AppEntry.
    fn insert_and_modify_entry<F>(&mut self, app: &App, cohort: &Option<Cohort>, modify: F)
    where
        F: Fn(&mut AppEntry),
    {
        for existing_entry in self.app_entries.iter_mut() {
            if existing_entry.app.id == app.id {
                // found an existing App in the Vec, so just run the closure on this AppEntry.
                modify(existing_entry);
                // and short-circuit out.
                return;
            }
        }

        // The App wasn't found, so add it to the list, after running the closure on a newly
        // generated AppEntry for this App.
        let mut app_entry = AppEntry::new(app, cohort);
        modify(&mut app_entry);
        self.app_entries.push(app_entry);
    }

    /// This function adds an update check for the given App, in the given Cohort.  This function is
    /// an idempotent accumulator, in that it only once adds the App with it's associated Cohort to
    /// the request.  Afterward, it just marks the App as needing an update check.
    pub fn add_update_check(mut self, app: &App, cohort: &Option<Cohort>) -> Self {
        self.insert_and_modify_entry(app, cohort, |entry| {
            entry.update_check = true;
        });
        self
    }

    /// This function adds a Ping for the given App, in the given Cohort.  This function is an
    /// idempotent accumulator, in that it only once adds the App with it's associated Cohort to the
    /// request.  Afterward, it just adds the Ping to the App.
    pub fn add_ping(mut self, app: &App, cohort: &Option<Cohort>, ping: &Ping) -> Self {
        self.insert_and_modify_entry(app, cohort, |entry| {
            entry.ping = Some(ping.clone());
        });
        self
    }

    /// This function adds an Event for the given App, in the given Cohort.  This function is an
    /// idempotent accumulator, in that it only once adds the App with it's associated Cohort to the
    /// request.  Afterward, it just adds the Event to the App.
    pub fn add_event(mut self, app: &App, cohort: &Option<Cohort>, event: &Event) -> Self {
        self.insert_and_modify_entry(app, cohort, |entry| {
            entry.events.push(event.clone());
        });
        self
    }

    /// This function constructs the protocol::request::Request object from this Builder.
    ///
    /// Note that the builder is consumed in the process, and cannot be used afterward.
    pub fn build(self) -> Result<http::Request<hyper::Body>> {
        self.build_intermediate().into()
    }

    /// Helper function that constructs the request body from the builder.
    fn build_intermediate(self) -> Intermediate {
        let mut headers = vec![
            // Set the content-type to be JSON.
            (http::header::CONTENT_TYPE.as_str(), "application/json".to_string()),
            // The updater name header is always set directly from the name in the configuration
            (HEADER_UPDATER_NAME, self.config.updater.name.clone()),
            // The interactivity header is set based on the source of the request that's set in
            // the request params
            (
                HEADER_INTERACTIVITY,
                match self.params.source {
                    InstallSource::OnDemand => "fg".to_string(),
                    InstallSource::ScheduledTask => "bg".to_string(),
                },
            ),
        ];
        // And the app id header is based on the first app id in the request.
        // TODO: Send all app ids, or only send the first based on configuration.
        if let Some(main_app) = self.app_entries.first() {
            headers.push((HEADER_APP_ID, main_app.app.id.clone()));
        }

        let apps = self.app_entries.into_iter().map(|entry| ProtocolApp::from(entry)).collect();

        Intermediate {
            uri: self.config.service_url.clone(),
            headers,
            body: RequestWrapper {
                request: Request {
                    protocol_version: PROTOCOL_V3.to_string(),
                    updater: self.config.updater.name.clone(),
                    updater_version: self.config.updater.version.to_string(),
                    install_source: self.params.source.clone(),
                    is_machine: true,
                    os: self.config.os.clone(),
                    apps,
                },
            },
        }
    }
}

/// As the name implies, this is an itermediate that can be used to construct an http::Request from
/// the data that's in the Builder.  It allows for type-aware inspection of the constructed protcol
/// request, as well as the full construction of the http request (uri, headers, body).
///
/// This struct owns all of it's data, so that they can be moved directly into the constructed http
/// request.
struct Intermediate {
    /// The URI for the http request.
    uri: String,

    /// The http request headers, in key:&str=value:String pairs
    headers: Vec<(&'static str, String)>,

    /// The request body, still in object form as a RequestWrapper
    body: RequestWrapper,
}

impl From<Intermediate> for Result<http::Request<hyper::Body>> {
    fn from(intermediate: Intermediate) -> Self {
        let mut builder = hyper::Request::get(intermediate.uri);
        for (key, value) in intermediate.headers {
            builder.header(key, value);
        }

        let body = serde_json::to_string(&intermediate.body)?;
        let request = builder.body(body.into())?;
        Ok(request)
    }
}