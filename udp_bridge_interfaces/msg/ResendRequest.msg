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
# this message changes. The receiver wraps decode in a try/catch and
# logs+drops on deserialization failure rather than terminating, so a
# transient mismatch does not crash the bridge. Same contract as
# MessageInternal.msg.
string connection_id
