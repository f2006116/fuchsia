// Not sure why this is needed: without it, the compiler will report that practically all of the
// code here is unused.
#![allow(unused)]

//! # Health-checking inspect node.
//!
//! Health installs a health checking inspect node.  This node reports the current program's health
//! status in the form of an enumeration string and, in case of an unhealthy status, a free-form
//! message.
//!
//! Possible statuses are as follows:
//!
//! - `OK`, the Node is HEALTHY
//! - `STARTING_UP`, the node is not yet HEALTHY
//! - `UNHEALTHY`, the node is NOT HEALTHY (the program is required to provide a status message).
//! - any other value, the node is NOT HEALTHY.
//!
//! # Usage
//!
//! To use the health checker one must first obtain a `fuchsia_inspect::Node` to add the health
//! information into. Once that is available, use `fuchsia_inspect::health::Node::new(...)` to
//! add a standardized health checker.
//!
//! # Examples
//!
//! ```
//! use fuchsia_inspect as inspect;
//! use fuchsia_inspect::health;
//!
//! let mut inspector = /* the inspector of your choice */
//! let mut root = inspector.root();  // Or perhaps a different Inspect Node of your choice.
//! let mut health = health::Node::new(root);
//!
//! health.set_ok();
//! // ...
//! health.set_unhealthy("I am not feeling well."); // Report an error
//! // ...
//! health.set_ok(); // The component is healthy again.
//! ```

use {
    super::{Inspector, Property, StringProperty},
    std::sync::{Arc, Mutex},
};

// The metric node name, as exposed by the health checker.
const FUCHSIA_INSPECT_HEALTH: &str = "fuchsia.inspect.Health";

const STATUS_PROPERTY_KEY: &str = "status";

const MESSAGE_PROPERTY_KEY: &str = "message";

/// Predefined statuses, per the Inspect health specification.  Note that the specification
/// also allows custom string statuses.
#[derive(Debug, PartialEq, Eq)]
enum Status {
    /// The health checker is available, but has not been initialized with program status yet.
    StartingUp,
    /// The program reports unhealthy status.  The program MUST provide a status message if reporting
    /// unhealthy.
    Unhealthy,
    /// The program reports healthy operation.  The definition of healthy is up to the program.
    Ok,
}

impl ToString for Status {
    fn to_string(&self) -> String {
        String::from(match self {
            Status::StartingUp => "STARTING_UP",
            Status::Unhealthy => "UNHEALTHY",
            Status::Ok => "OK",
        })
    }
}

/// Contains subsystem health information.  A global instance of Health is used implicitly
/// if the user calls the functions `init()`, `ok()` and `unhealthy(...)`.
///
/// Use as: ```fuchsia_inspect::health::Node``.
pub struct Node {
    // The generic inspect node that hosts the health metric.
    node: super::Node,

    // The health status of the property
    status: StringProperty,

    // The detailed status message, filled out in case the health status is not OK.
    message: Option<StringProperty>,
}

impl Node {
    /// Creates a new health checking node as a child of `parent`.  The initial observed state
    /// is `STARTING_UP`, and remains so until the programs call one of `set_ok` or `set_unhealthy`.
    pub fn new(parent: &super::Node) -> Self {
        let node = parent.create_child(FUCHSIA_INSPECT_HEALTH);
        let status = node.create_string(STATUS_PROPERTY_KEY, Status::StartingUp.to_string());
        let message = None;
        Node { node, status, message }
    }

    // Sets the health status to `OK`.
    pub fn set_ok(&mut self) {
        self.set_status_enum(Status::Ok, None);
    }

    // Sets the health status to `UNHEALTHY`.  A `message` that explains why the node is healthy
    // MUST be given.
    pub fn set_unhealthy(&mut self, message: &str) {
        self.set_status_enum(Status::Unhealthy, Some(message));
    }

    // Sets the health status from the supplied `status` and `message`.  Panics if setting invalid
    // status, e.g. setting `UNHEALTHY` without a message.
    fn set_status_enum(&mut self, status: Status, message: Option<&str>) {
        assert!(status != Status::Unhealthy || message != None, "UNHEALTHY must have a message.");
        self.set_status(&status.to_string(), message);
    }

    // Sets an arbitrary status and an arbitrary (optional) message into the health report.
    // Prefer setting standard status using one of the predefined API methods.  This one will
    // allow you to set whatever you want.
    fn set_status(&mut self, status: &str, message: Option<&str>) {
        self.status.set(status);
        match (&self.message, message) {
            (_, None) => self.message = None,
            (Some(m), Some(n)) => m.set(n),
            (None, Some(n)) => {
                self.message = Some(self.node.create_string(MESSAGE_PROPERTY_KEY, n))
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use std::convert::TryFrom;

    use crate::assert_inspect_tree;
    use crate::reader::NodeHierarchy;
    use crate::reader::Property;

    use super::*;

    #[test]
    fn health_checker_lifecycle() {
        let inspector = Inspector::new();
        let mut root = inspector.root();
        // In the beginning, the inspector has no stats.
        assert_inspect_tree!(inspector, root: contains {});

        let mut health = Node::new(root);
        assert_inspect_tree!(inspector,
        root: contains {
            "fuchsia.inspect.Health": {
                status: "STARTING_UP",
            }
        });

        health.set_ok();
        assert_inspect_tree!(inspector,
        root: contains {
            "fuchsia.inspect.Health": {
                status: "OK",
            }
        });

        health.set_unhealthy("Bad state");
        assert_inspect_tree!(inspector,
        root: contains {
            "fuchsia.inspect.Health": {
                status: "UNHEALTHY",
                message: "Bad state",
            }
        });

        // Verify that the message changes.
        health.set_unhealthy("Another bad state");
        assert_inspect_tree!(inspector,
        root: contains {
            "fuchsia.inspect.Health": {
                status: "UNHEALTHY",
                message: "Another bad state",
            }
        });

        // Also verifies that there is no more message.
        health.set_ok();
        assert_inspect_tree!(inspector,
        root: contains {
            "fuchsia.inspect.Health": {
                status: "OK",
            }
        });
    }
}
