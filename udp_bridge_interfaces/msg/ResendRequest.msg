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
# MessageInternal.msg). To bound the failure mode, the receiver
# (UDPBridge::decodeResendRequest) wraps the deserialize call in a
# try/catch and logs+drops on deserialization failure rather than
# letting the exception propagate to the executor and kill the bridge.
# Note: this catch lives at the decodeResendRequest site specifically;
# other decode* sites (notably decodeMessageInternal) do not yet have
# the same guard — don't generalize this claim until they do.
string connection_id
