# Internal message used to ask to resend missing packets
uint64[] missing_packets

# Identifier of the connection the request was sent on. The receiver
# uses this to route the resend response back via that single
# connection instead of fanning out across every active path to the
# remote (see issue #23 for the field motivation).
#
# Adding fields to this message changes the serialized wire layout.
# Mixed-version sender/receiver interoperation is NOT guaranteed by
# the schema alone — both endpoints must be redeployed together when
# this message changes (same coordinated-redeploy contract as
# MessageInternal.msg). The receiver (UDPBridge::decodeResendRequest)
# wraps the deserialize call in a try/catch primarily for diagnostic
# clarity: a deserialize throw on a stale-schema buffer surfaces as a
# specific WARN ("Failed to deserialize ResendRequest from <node>
# (<host>:<port>)") instead of UDPBridge::decode's outer catch-all
# emitting the generic "decoding error on packet of type N" ERROR.
# Executor survival is already provided by that outer catch-all in
# UDPBridge::decode (which wraps every decode* dispatch); the
# site-specific catch is about diagnosability of the
# coordinated-redeploy scenario, not survival.
# Note: this site-specific catch lives at decodeResendRequest only;
# other decode* sites (notably decodeMessageInternal) emit the more
# generic outer-catch message — don't generalize this claim until
# they have their own site-specific catches.
string connection_id
