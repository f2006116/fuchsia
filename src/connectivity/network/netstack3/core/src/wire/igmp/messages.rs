// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Implementation of IGMP Messages.

use packet::{BufferView, ParsablePacket, ParseMetadata};
use zerocopy::{AsBytes, ByteSlice, FromBytes, LayoutVerified, Unaligned};

use super::{
    make_v3_possible_floating_point, parse_v3_possible_floating_point, peek_message_type,
    IgmpMessage, IgmpResponseTimeV2, IgmpResponseTimeV3,
};
use crate::error::ParseError;
use crate::ip::Ipv4Addr;
use crate::wire::igmp::MessageType;
use crate::wire::records::{LimitedRecords, LimitedRecordsImpl, LimitedRecordsImplLayout};
use crate::wire::U16;

create_net_enum! {
    IgmpMessageType,
    MembershipQuery: MEMBERSHIP_QUERY = 0x11,
    MembershipReportV1: MEMBERSHIP_REPORT_V1 = 0x12,
    MembershipReportV2: MEMBERSHIP_REPORT_V2 = 0x16,
    MembershipReportV3: MEMBERSHIP_REPORT_V3 = 0x22,
    LeaveGroup: LEAVE_GROUP = 0x17,
}

macro_rules! impl_igmp_simple_message_type {
    ($type:ident, $code:tt, $fixed_header:ident) => {
        impl<B> MessageType<B> for $type {
            type FixedHeader = $fixed_header;
            const TYPE: IgmpMessageType = IgmpMessageType::$code;
            type MaxRespTime = ();
            declare_no_body!();
        }
    };
}

macro_rules! declare_no_body {
    () => {
        type VariableBody = ();

        fn parse_body<BV: BufferView<B>>(
            header: &Self::FixedHeader,
            mut bytes: BV,
        ) -> Result<Self::VariableBody, ParseError>
        where
            B: ByteSlice,
        {
            if bytes.len() != 0 {
                Err(ParseError::NotExpected)
            } else {
                Ok(())
            }
        }

        fn body_bytes(body: &Self::VariableBody) -> &[u8]
        where
            B: ByteSlice,
        {
            &[]
        }
    };
}

/// IGMPv2 Membership Query message.
///
/// `IgmpMembershipQueryV2` implements `MessageType`, providing the intended
/// behavior for IGMPv2 Membership Queries as defined in [RFC 2236].
///
/// There are two sub-types of Membership Query messages:
/// - General Query, used to learn which groups have members on an
///   attached network.
/// - Group-Specific Query, used to learn if a particular group
///   has any members on an attached network.
///
/// These two messages are differentiated by the Group Address, as
/// described in [RFC 2236 section 1.4].
///
/// [RFC 2236]: https://tools.ietf.org/html/rfc2236
/// [RFC 2236 section 1.4]: https://tools.ietf.org/html/rfc2236#section-1.4
#[derive(Copy, Clone, Debug)]
pub(crate) struct IgmpMembershipQueryV2;

impl<B> MessageType<B> for IgmpMembershipQueryV2 {
    type FixedHeader = Ipv4Addr;
    type MaxRespTime = IgmpResponseTimeV2;
    const TYPE: IgmpMessageType = IgmpMessageType::MembershipQuery;

    declare_no_body!();
}

/// Fixed information in IGMPv3 Membership Queries.
///
/// A `MembershipQueryData` struct represents the fixed data in IGMPv3
/// Membership Queries.
/// It is defined as the `FixedHeader` type for `IgmpMembershipQueryV3`.
#[derive(Copy, Clone, Debug, AsBytes, FromBytes, Unaligned)]
#[repr(C)]
pub(crate) struct MembershipQueryData {
    group_address: Ipv4Addr,
    sqrv: u8,
    qqic: u8,
    number_of_sources: U16,
}

impl MembershipQueryData {
    const S_FLAG: u8 = (1 << 3);
    const QRV_MSK: u8 = 0x07;

    /// The default querier robutstness variable, as defined in
    /// [RFC 3376 section 8.1].
    ///
    /// [RFC 3376 section 8.1]: https://tools.ietf.org/html/rfc3376#section-8.1
    pub(crate) const DEFAULT_QRV: u8 = 2;
    /// The default Query Interval, as defined in [RFC 3376 section 8.2].
    ///
    /// [RFC 3376 section 8.2]: https://tools.ietf.org/html/rfc3376#section-8.2
    pub(crate) const DEFAULT_QUERY_INTERVAL: std::time::Duration =
        std::time::Duration::from_secs(1250);

    pub(crate) fn number_of_sources(self) -> u16 {
        self.number_of_sources.get()
    }

    pub(crate) fn set_number_of_sources(&mut self, val: u16) {
        self.number_of_sources = U16::new(val);
    }

    /// Gets value of `S Flag`.
    ///
    /// Indicates to any receiving multicast routers that they are to suppress
    /// the normal timer updates they perform upon hearing a Query. It does not,
    /// however, suppress the querier election or the normal "host-side"
    /// processing of a Query that a router may be required to perform as a
    /// consequence of itself being a group member.
    pub(crate) fn suppress_router_side_processing(self) -> bool {
        (self.sqrv & Self::S_FLAG) != 0
    }

    pub(crate) fn set_suppress_router_side_processing(&mut self, val: bool) {
        if val {
            self.sqrv |= Self::S_FLAG;
        } else {
            self.sqrv &= !(Self::S_FLAG);
        }
    }

    /// Gets querier robustness variable (QRV).
    ///
    /// If non-zero, the QRV field contains the *Robustness Variable* value
    /// used by the querier, i.e., the sender of the Query.  If the querier's
    /// *Robustness Variable* exceeds 7, the maximum value of the QRV field,
    /// the QRV is set to zero.  Routers adopt the QRV value from the most
    /// recently received Query as their own *Robustness Variable* value,
    /// unless that most recently received QRV was zero, in which case the
    /// receivers use the default *Robustness Variable* value specified in
    /// [RFC 3376 section 8.1] (defined as `DEFAULT_QRV`) or a statically
    /// configured value.
    ///
    /// [RFC 3376 section 8.1]: https://tools.ietf.org/html/rfc3376#section-8.1
    fn querier_robustness_variable(self) -> u8 {
        self.sqrv & Self::QRV_MSK
    }

    pub(crate) fn set_querier_robustness_variable(&mut self, val: u8) {
        self.sqrv |= (val & Self::QRV_MSK);
    }

    /// Gets the querier's query interval (QQI).
    ///
    /// Multicast routers that are not the current querier adopt the QQI
    /// value from the most recently received Query as their own *Query
    /// Interval* value, unless that most recently received QQI was zero, in
    /// which case the receiving routers use the default *Query Interval*
    /// value specified in [RFC 3376 section 8.2], defined as
    /// `DEFAULT_QUERY_INTERVAL`.
    ///
    /// [RFC 3376 section 8.2]: https://tools.ietf.org/html/rfc3376#section-8.2
    pub(crate) fn querier_query_interval(self) -> std::time::Duration {
        // qqic is represented in a packed floating point format and interpreted
        // as units of seconds.
        std::time::Duration::from_secs(parse_v3_possible_floating_point(self.qqic).into())
    }

    /// Sets the querier's query interval to given `itv`.
    ///
    /// # Panics
    /// panics if the given interval is not representable by the underlying
    /// floating point code mechanics, i.e., value in seconds of informed
    /// `itv` is > *31744*
    pub(crate) fn set_querier_query_interval(&mut self, itv: std::time::Duration) {
        self.qqic = make_v3_possible_floating_point(itv.as_secs() as u32).unwrap()
    }
}

/// IGMPv3 Membership Query message.
///
/// `IgmpMembershipQueryV3` implements `MessageType`, providing the intended
/// behavior for IGMPv3 Membership Queries as defined in
/// [RFC 3376 section 4.1].
///
/// Membership Queries are sent by IP multicast routers to query the
/// multicast reception state of neighboring interfaces.
///
/// [RFC 3376 section 4.1]: https://tools.ietf.org/html/rfc3376#section-4.1
#[derive(Copy, Clone, Debug)]
pub(crate) struct IgmpMembershipQueryV3;

impl<B> MessageType<B> for IgmpMembershipQueryV3 {
    type FixedHeader = MembershipQueryData;
    type VariableBody = LayoutVerified<B, [Ipv4Addr]>;
    type MaxRespTime = IgmpResponseTimeV3;
    const TYPE: IgmpMessageType = IgmpMessageType::MembershipQuery;

    fn parse_body<BV: BufferView<B>>(
        header: &Self::FixedHeader,
        mut bytes: BV,
    ) -> Result<Self::VariableBody, ParseError>
    where
        B: ByteSlice,
    {
        bytes
            .take_slice_front::<Ipv4Addr>(header.number_of_sources() as usize)
            .ok_or(ParseError::Format)
    }

    fn body_bytes(body: &Self::VariableBody) -> &[u8]
    where
        B: ByteSlice,
    {
        body.bytes()
    }
}

/// Fixed information in IGMPv3 Membership Reports.
///
/// A `MembershipReportV3Data` struct represents the fixed data in IGMPv3
/// Membership Reports.
/// It is defined as the `FixedHeader` type for `IgmpMembershipReportV3`.
#[derive(Copy, Clone, Debug, AsBytes, FromBytes, Unaligned)]
#[repr(C)]
pub(crate) struct MembershipReportV3Data {
    _reserved: [u8; 2],
    number_of_group_records: U16,
}

impl MembershipReportV3Data {
    pub(crate) fn number_of_group_records(self) -> u16 {
        self.number_of_group_records.get()
    }

    pub(crate) fn set_number_of_group_records(&mut self, val: u16) {
        self.number_of_group_records = U16::new(val);
    }
}

/// Group Record Types as defined in [RFC 3376 section 4.2.12]
///
/// [RFC 3376 section 4.2.12]: https://tools.ietf.org/html/rfc3376#section-4.2.12
create_net_enum! {
    IgmpGroupRecordType,
    ModeIsInclude: MODE_IS_INCLUDE = 0x01,
    ModeIsExclude: MODE_IS_EXCLUDE = 0x02,
    ChangeToIncludeMode: CHANGE_TO_INCLUDE_MODE = 0x03,
    ChangeToExcludeMode: CHANGE_TO_EXCLUDE_MODE = 0x04,
    AllowNewSources : ALLOW_NEW_SOURCES = 0x05,
    BlockOldSources: BLOCK_OLD_SOURCES = 0x06,
}

/// Fixed information for IGMPv3 Membership Report's Group Records.
///
/// A `GroupRecordHeader` struct represents the fixed header of a Group Record
/// in IGMPv3 Membership Report messages as defined in [RFC 3376 section 4.2.4].
///
/// The `GroupRecordHeader` is followed by a series of source IPv4 addresses.
///
/// [RFC 3376 section 4.2.4]: https://tools.ietf.org/html/rfc3376#section-4.2.4
#[derive(Copy, Clone, Debug, AsBytes, FromBytes, Unaligned)]
#[repr(C)]
pub(crate) struct GroupRecordHeader {
    record_type: u8,
    aux_data_len: u8,
    number_of_sources: U16,
    multicast_address: Ipv4Addr,
}

impl GroupRecordHeader {
    pub(crate) fn number_of_sources(self) -> u16 {
        self.number_of_sources.get()
    }

    pub(crate) fn set_number_of_sources(&mut self, val: u16) {
        self.number_of_sources = U16::new(val);
    }

    pub(crate) fn record_type(self) -> Option<IgmpGroupRecordType> {
        IgmpGroupRecordType::from_u8(self.record_type)
    }

    pub(crate) fn set_record_type(&mut self, val: IgmpGroupRecordType) {
        self.record_type = val.into();
    }
}

/// Wire representation of an IGMPv3 Membership Report's Group Records.
///
/// A `GroupRecord` struct is composed of a fixed part provided by
/// `GroupRecordHeader` and a variable part, which is a list of IPv4 addresses.
///
/// Each Group Record is a block of fields containing information
/// pertaining to the sender's membership in a single multicast group on
/// the interface from which the Report is sent.
///
/// The structure of a Group Record includes Auxiliary Data, as defined in
/// [RFC 3376 section 4.2.10]. As the information in Auxiliary Data is supposed
/// to be ignored, when parsing a `GroupRecord` struct from a buffer, the
/// information in Auxiliary Data, if any, is discarded.
///
/// [RFC 3376 section 4.2.10]: https://tools.ietf.org/html/rfc3376#section-4.2.10
pub(crate) struct GroupRecord<B> {
    header: LayoutVerified<B, GroupRecordHeader>,
    sources: LayoutVerified<B, [Ipv4Addr]>,
}

/// IGMPv3 Membership Report message.
///
/// `IgmpMembershipReportV3` implements `MessageType`, providing the intended
/// behavior for IGMPv3 Membership Reports as defined in
/// [RFC 3376 section 4.2].
///
/// Version 3 Membership Reports are sent by IP systems to report (to
/// neighboring routers) the current multicast reception state, or
/// changes in the multicast reception state, of their interfaces.
///
/// [RFC 3376 section 4.2]: https://tools.ietf.org/html/rfc3376#section-4.2
#[derive(Copy, Clone, Debug)]
pub(crate) struct IgmpMembershipReportV3;

impl<B> MessageType<B> for IgmpMembershipReportV3 {
    type FixedHeader = MembershipReportV3Data;
    type VariableBody = LimitedRecords<B, IgmpMembershipReportV3>;
    type MaxRespTime = ();
    const TYPE: IgmpMessageType = IgmpMessageType::MembershipReportV3;

    fn parse_body<BV: BufferView<B>>(
        header: &Self::FixedHeader,
        mut bytes: BV,
    ) -> Result<Self::VariableBody, ParseError>
    where
        B: ByteSlice,
    {
        LimitedRecords::<_, _>::parse_with_context(
            bytes.into_rest(),
            header.number_of_group_records().into(),
        )
    }

    fn body_bytes(body: &Self::VariableBody) -> &[u8]
    where
        B: ByteSlice,
    {
        body.bytes()
    }
}

impl LimitedRecordsImplLayout for IgmpMembershipReportV3 {
    type Error = ParseError;

    const EXACT_LIMIT_ERROR: Option<ParseError> = Some(ParseError::Format);
}

impl<'a> LimitedRecordsImpl<'a> for IgmpMembershipReportV3 {
    type Record = GroupRecord<&'a [u8]>;

    fn parse<BV: BufferView<&'a [u8]>>(
        data: &mut BV,
    ) -> Result<Option<Option<GroupRecord<&'a [u8]>>>, ParseError> {
        let header = data
            .take_obj_front::<GroupRecordHeader>()
            .ok_or_else(debug_err_fn!(ParseError::Format, "Can't take group record header"))?;
        let sources = data
            .take_slice_front::<Ipv4Addr>(header.number_of_sources().into())
            .ok_or_else(debug_err_fn!(ParseError::Format, "Can't group record sources"))?;
        // every record may have aux_data_len 32-bit words at the end.
        // we need to update our buffer view to reflect that.
        let _ = data
            .take_front(usize::from(header.aux_data_len) * 4)
            .ok_or_else(debug_err_fn!(ParseError::Format, "Can't skip auxiliary data"))?;

        Ok(Some(Some(Self::Record { header, sources })))
    }
}

/// IGMPv1 Membership Report message.
///
/// `IgmpMembershipReportV1` implements `MessageType`, providing the intended
/// behavior for IGMPv1 Membership Reports as defined in [RFC 1112].
///
/// In a Host Membership Report message, the group address field (expressed in
/// `FixedHeader`) holds the IP host group address of the group being reported.
///
/// Hosts respond to a Query by generating Host Membership Reports, reporting
/// each host group to which they belong on the network interface from which the
/// Query was received.
///
/// [RFC 1112]: https://tools.ietf.org/html/rfc1112
#[derive(Debug)]
pub(crate) struct IgmpMembershipReportV1;

impl_igmp_simple_message_type!(IgmpMembershipReportV1, MembershipReportV1, Ipv4Addr);

/// IGMPv2 Membership Report message.
///
/// `IgmpMembershipReportV2` implements `MessageType`, providing the intended
/// behavior for IGMPv2 Membership Reports as defined in [RFC 2236].
///
/// In a Membership Report message, the group address field (expressed in
/// `FixedHeader`) holds the IP multicast group address of the group being
/// reported.
///
/// Hosts respond to a Query by generating Host Membership Reports, reporting
/// each host group to which they belong on the network interface from which the
/// Query was received.
///
/// [RFC 2236]: https://tools.ietf.org/html/rfc2236
#[derive(Debug)]
pub(crate) struct IgmpMembershipReportV2;

impl_igmp_simple_message_type!(IgmpMembershipReportV2, MembershipReportV2, Ipv4Addr);

/// IGMP Leave Group message.
///
/// `IgmpLeaveGroup` implements `MessageType`, providing the intended behavior
/// for IGMP LeaveGroup as defined in [RFC 2236].
///
/// In a Leave Group message, the group address field (expressed in
/// `FixedHeader`) holds the IP multicast group address of the group being
/// left.
///
/// When a host leaves a multicast group, if it was the last host to reply to a
/// Query with a Membership Report for that group, it SHOULD send a Leave Group
/// message to the all-routers multicast group (224.0.0.2).
///
/// [RFC 2236]: https://tools.ietf.org/html/rfc2236
#[derive(Debug)]
pub(crate) struct IgmpLeaveGroup;

impl_igmp_simple_message_type!(IgmpLeaveGroup, LeaveGroup, Ipv4Addr);

/// An IGMP packet with a dynamic message type.
///
/// Each enum variant contains an `IgmpMessage` of
/// the appropriate static type, making it easier to call `parse` without
/// knowing the message type ahead of time while still getting the benefits of a
/// statically-typed packet struct after parsing is complete.
pub(crate) enum IgmpPacket<B> {
    MembershipQueryV2(IgmpMessage<B, IgmpMembershipQueryV2>),
    MembershipQueryV3(IgmpMessage<B, IgmpMembershipQueryV3>),
    MembershipReportV1(IgmpMessage<B, IgmpMembershipReportV1>),
    MembershipReportV2(IgmpMessage<B, IgmpMembershipReportV2>),
    MembershipReportV3(IgmpMessage<B, IgmpMembershipReportV3>),
    LeaveGroup(IgmpMessage<B, IgmpLeaveGroup>),
}

impl<B: ByteSlice> ParsablePacket<B, ()> for IgmpPacket<B> {
    type Error = ParseError;

    fn parse_metadata(&self) -> ParseMetadata {
        use self::IgmpPacket::*;
        match self {
            MembershipQueryV2(p) => p.parse_metadata(),
            MembershipQueryV3(p) => p.parse_metadata(),
            MembershipReportV1(p) => p.parse_metadata(),
            MembershipReportV2(p) => p.parse_metadata(),
            MembershipReportV3(p) => p.parse_metadata(),
            LeaveGroup(p) => p.parse_metadata(),
        }
    }

    fn parse<BV: BufferView<B>>(mut buffer: BV, args: ()) -> Result<Self, ParseError> {
        macro_rules! mtch {
            ($buffer:expr, $args:expr, $( ($code:ident, $long:tt) => $type:ty, $variant:ident )*) => {
                match peek_message_type($buffer.as_ref())? {
                    $( (IgmpMessageType::$code, $long) => {
                        let packet = <IgmpMessage<B,$type> as ParsablePacket<_, _>>::parse($buffer, $args)?;
                        IgmpPacket::$variant(packet)
                    })*,
                }
            }
        }

        Ok(mtch!(
            buffer,
            args,
            (MembershipQuery, false) => IgmpMembershipQueryV2, MembershipQueryV2
            (MembershipQuery, true) => IgmpMembershipQueryV3, MembershipQueryV3
            (MembershipReportV1, _) => IgmpMembershipReportV1, MembershipReportV1
            (MembershipReportV2, _) => IgmpMembershipReportV2, MembershipReportV2
            (MembershipReportV3, _) => IgmpMembershipReportV3, MembershipReportV3
            (LeaveGroup, _) => IgmpLeaveGroup, LeaveGroup
        ))
    }
}

#[cfg(test)]
mod tests {
    use super::super::IgmpMessage;
    use super::*;

    use packet::serialize::Serializer;
    use packet::ParseBuffer;

    use crate::testutil::set_logger_for_test;
    use crate::wire::igmp::testdata::*;
    use std::fmt::Debug;

    const ALL_BUFFERS: [&'static [u8]; 6] = [
        igmp_router_queries::v2::QUERY,
        igmp_router_queries::v3::QUERY,
        igmp_reports::v1::MEMBER_REPORT,
        igmp_reports::v2::MEMBER_REPORT,
        igmp_reports::v3::MEMBER_REPORT,
        igmp_leave_group::LEAVE_GROUP,
    ];

    fn serialize_to_bytes<B: ByteSlice + Debug, M: MessageType<B> + Debug>(
        igmp: &IgmpMessage<B, M>,
    ) -> Vec<u8> {
        M::body_bytes(&igmp.body)
            .encapsulate(igmp.builder())
            .serialize_outer()
            .unwrap()
            .as_ref()
            .to_vec()
    }

    fn test_parse_and_serialize<
        M: for<'a> MessageType<&'a [u8]> + Debug,
        F: for<'a> FnOnce(&IgmpMessage<&'a [u8], M>),
    >(
        mut req: &[u8],
        check: F,
    ) {
        let orig_req = &req[..];

        let igmp = req.parse_with::<_, IgmpMessage<_, M>>(()).unwrap();
        check(&igmp);

        let data = serialize_to_bytes(&igmp);
        assert_eq!(&data[..], orig_req);
    }

    #[test]
    fn membership_query_v2_parse_and_serialize() {
        set_logger_for_test();
        test_parse_and_serialize::<IgmpMembershipQueryV2, _>(
            igmp_router_queries::v2::QUERY,
            |igmp| {
                assert_eq!(
                    *igmp.header,
                    Ipv4Addr::new(igmp_router_queries::v2::HOST_GROUP_ADDRESS)
                );
                assert_eq!(igmp.prefix.max_resp_code, igmp_router_queries::v2::MAX_RESP_CODE);
            },
        );
    }

    #[test]
    fn membership_query_v3_parse_and_serialize() {
        set_logger_for_test();
        test_parse_and_serialize::<IgmpMembershipQueryV3, _>(
            igmp_router_queries::v3::QUERY,
            |igmp| {
                assert_eq!(igmp.prefix.max_resp_code, igmp_router_queries::v3::MAX_RESP_CODE);
                assert_eq!(
                    igmp.header.group_address,
                    Ipv4Addr::new(igmp_router_queries::v3::GROUP_ADDRESS)
                );
                assert_eq!(
                    igmp.header.number_of_sources(),
                    igmp_router_queries::v3::NUMBER_OF_SOURCES
                );
                assert_eq!(
                    igmp.header.suppress_router_side_processing(),
                    igmp_router_queries::v3::SUPPRESS_ROUTER_SIDE
                );
                assert_eq!(igmp.header.querier_robustness_variable(), igmp_router_queries::v3::QRV);
                assert_eq!(
                    igmp.header.querier_query_interval().as_secs() as u32,
                    igmp_router_queries::v3::QQIC_SECS
                );
                assert_eq!(igmp.body.len(), igmp_router_queries::v3::NUMBER_OF_SOURCES as usize);
                assert_eq!(igmp.body[0], Ipv4Addr::new(igmp_router_queries::v3::SOURCE));
            },
        );
    }

    #[test]
    fn membership_report_v3_parse_and_serialize() {
        use igmp_reports::v3::*;

        set_logger_for_test();

        test_parse_and_serialize::<IgmpMembershipReportV3, _>(MEMBER_REPORT, |igmp| {
            assert_eq!(igmp.header.number_of_group_records(), NUMBER_OF_RECORDS);
            assert_eq!(igmp.prefix.max_resp_code, MAX_RESP_CODE);
            let mut iter = igmp.body.iter();
            // look at first group record:
            let rec1 = iter.next().unwrap();
            assert_eq!(rec1.header.number_of_sources(), NUMBER_OF_SOURCES_1);
            assert_eq!(rec1.header.record_type, RECORD_TYPE_1);
            assert_eq!(rec1.header.multicast_address, Ipv4Addr::new(MULTICAST_ADDR_1));
            assert_eq!(rec1.header.record_type(), Some(IgmpGroupRecordType::ModeIsInclude));
            assert_eq!(rec1.sources.len(), NUMBER_OF_SOURCES_1 as usize);
            assert_eq!(rec1.sources[0], Ipv4Addr::new(SRC_1_1));
            assert_eq!(rec1.sources[1], Ipv4Addr::new(SRC_1_2));

            // look at second group record:
            let rec2 = iter.next().unwrap();
            assert_eq!(rec2.header.number_of_sources(), NUMBER_OF_SOURCES_2);
            assert_eq!(rec2.header.record_type, RECORD_TYPE_2);
            assert_eq!(rec2.header.multicast_address, Ipv4Addr::new(MULTICAST_ADDR_2));
            assert_eq!(rec2.header.record_type(), Some(IgmpGroupRecordType::ModeIsExclude));
            assert_eq!(rec2.sources.len(), NUMBER_OF_SOURCES_2 as usize);
            assert_eq!(rec2.sources[0], Ipv4Addr::new(SRC_2_1));

            // assert that no other records came in:
            assert_eq!(iter.next().is_none(), true);
        });
    }

    #[test]
    fn membership_report_v1_parse_and_serialize() {
        use igmp_reports::v1;
        set_logger_for_test();
        test_parse_and_serialize::<IgmpMembershipReportV1, _>(v1::MEMBER_REPORT, |igmp| {
            assert_eq!(*igmp.header, Ipv4Addr::new(v1::GROUP_ADDRESS));
        });
    }

    #[test]
    fn membership_report_v2_parse_and_serialize() {
        use igmp_reports::v2;
        set_logger_for_test();
        test_parse_and_serialize::<IgmpMembershipReportV2, _>(v2::MEMBER_REPORT, |igmp| {
            assert_eq!(*igmp.header, Ipv4Addr::new(v2::GROUP_ADDRESS));
        });
    }

    #[test]
    fn leave_group_parse_and_serialize() {
        set_logger_for_test();
        test_parse_and_serialize::<IgmpLeaveGroup, _>(igmp_leave_group::LEAVE_GROUP, |igmp| {
            assert_eq!(*igmp.header, Ipv4Addr::new(igmp_leave_group::GROUP_ADDRESS));
        });
    }

    #[test]
    fn test_unknown_type() {
        let mut buff = &mut igmp_invalid_buffers::UNKNOWN_TYPE;
        let packet = buff.parse_with::<_, IgmpPacket<_>>(());
        // we don't use expect_err here because IgmpPacket does not implement
        // std::fmt::Debug
        assert_eq!(packet.is_err(), true);
    }

    #[test]
    fn test_full_parses() {
        for buff in ALL_BUFFERS.iter_mut() {
            let orig_req = &buff[..];
            let packet = buff.parse_with::<_, IgmpPacket<_>>(()).unwrap();
            let msg_type = match (packet) {
                IgmpPacket::MembershipQueryV2(p) => p.prefix.msg_type,
                IgmpPacket::MembershipQueryV3(p) => p.prefix.msg_type,
                IgmpPacket::MembershipReportV1(p) => p.prefix.msg_type,
                IgmpPacket::MembershipReportV2(p) => p.prefix.msg_type,
                IgmpPacket::MembershipReportV3(p) => p.prefix.msg_type,
                IgmpPacket::LeaveGroup(p) => p.prefix.msg_type,
            };
            assert_eq!(msg_type, orig_req[0]);
        }
    }

    #[test]
    fn test_partial_parses() {
        // parsing a part of the buffer should always result in errors and
        // nothing panics.
        for buff in ALL_BUFFERS.iter() {
            for i in 0..buff.len() {
                let partial_buff = &mut &buff[0..i];
                let packet = partial_buff.parse_with::<_, IgmpPacket<_>>(());
                assert_eq!(packet.is_err(), true)
            }
        }
    }
}
