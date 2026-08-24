#ifndef UDP_BRIDGE_REMOTE_IDENTITY_H
#define UDP_BRIDGE_REMOTE_IDENTITY_H

#include <set>
#include <string>
#include <vector>

#include "udp_bridge/packet.h"

namespace udp_bridge
{

/// Identity of a statically-configured remote (issues #51, #67).
///
/// There is exactly ONE namespace for remote identity in this bridge: the
/// name a bridge calls itself on the wire. It is what `WrappedPacket`
/// carries as `source_node`, what `remote_nodes_` is keyed by when a packet
/// arrives, and what the relay loop rule compares against.
///
/// A `remotes_list` entry IS that name. It is simultaneously the label that
/// spells the remote's parameter paths (`remotes.<label>.connections_list`)
/// and the wire identity everything static configuration creates is keyed
/// by (`remote_nodes_`, `subscribers_[topic].remote_details`). So the
/// constraint that was always implicit is now the whole rule: **your label
/// for a peer must be the name that peer calls itself.**
///
/// ### Why there used to be two namespaces (issue #67)
///
/// The ROS 1 bridge had no labels. `remotes` was a nested parameter
/// dictionary iterated directly, and a remote's name was the dictionary
/// KEY (`remote_info.name = remote.first`), with an optional `name:` member
/// inside the block able to override it. ROS 2's parameter model is flat —
/// there are no nested dictionaries — so the port had to manufacture
/// `remotes_list` plus `remotes.<label>.*` paths to express the same shape.
/// The label is that artefact of the port; the `name:` override rode along
/// with it.
///
/// Issue #51 found the bug the two namespaces caused: `on_configure` keyed
/// everything by the label while `remotes.<label>.name` was declared and
/// never read, so a remote whose own name differed from the hub's label for
/// it occupied two keys — the label (from `on_configure`) and its wire name
/// (created on first packet). The relay loop rule, which compares the wire
/// `source_node` against `remote_details` keys, then matched nothing and
/// the hub relayed the sender's own traffic straight back to it, including
/// in a single-remote configuration where relay is supposed to be inert.
/// #51 fixed it by resolving the identity here at configure time
/// (`remotes.<label>.name` when set, else the label).
///
/// Issue #67 retired the override instead: the split was never wanted, so
/// there is one string per remote again, as in ROS 1. This is a deliberate
/// retirement of a working ROS 1 feature, not the repair of an accident —
/// a `name:` key found in an old config did once do something. It is now
/// declared (so rclcpp surfaces the override at all) and read ONLY to
/// REFUSE a config that still sets it: a non-empty value fails
/// `on_configure`, on presence, with no carve-out for a value equal to its
/// label. Warning would leave the one shape that reintroduces the echo
/// above — a value differing from its label — running. See
/// `UDPBridge::on_configure`.
///
/// ### What retirement costs
///
/// An entry is now both a wire identity (at most
/// `maximum_node_name_length` bytes) AND a ROS 2 parameter-path segment,
/// and ROS 2 uses `.` to separate segments. A peer whose wire name
/// contains a `.` is still configurable — verified: `remotes_list:
/// ["boat.one"]` with the YAML nested as `remotes: boat: one: ...`
/// resolves `remotes.boat.one.connections_list` correctly — but its
/// parameters must then be written as if `boat` and `one` were separate
/// levels, which reads as a nested structure rather than one name.
/// Before #67 such a name could be given flatly via
/// `remotes.<label>.name`. No known configuration uses one; prefer names
/// without `.` so the config says what it means.

/// Validate every configured remote label, rejecting over-long labels, the
/// empty label, and any label equal to this bridge's own name.
///
/// A label longer than `maximum_node_name_length` is a configuration error
/// (issue #51). It cannot be carried on the wire, so the remote's packets
/// arrive stamped with a shortened `source_node` that never equals the
/// identity this table is keyed by: the relay loop rule matches nothing and
/// the hub echoes the sender's own traffic back to it. Truncating here
/// instead would ALSO let two labels differing only after the limit collapse
/// onto one wire identity, silently merging two remotes' connections and
/// rate limits. Reject it and say so.
///
/// A label equal to THIS bridge's own name is rejected for a second reason
/// (issue #51). Such an entry installs a `RemoteNode` for ourselves in
/// `remote_nodes_`, and once it is there `unwrap()` can no longer refuse
/// our own packets: its self-packet check sits in the not-found branch, so
/// a successful lookup walks straight past it and the bridge starts
/// treating its own traffic as a peer's. `RemoteNode`'s constructor does
/// assert on it, but an assert is compiled out of a release build, which is
/// where this would be met. Catch it here, before the RemoteNode exists.
///
/// The EMPTY label is rejected for a third (issue #51). `""` is a reserved
/// sentinel on the send path: an empty remote name there means "this is a
/// connection request, not a message to a named remote". A remote filed
/// under it is therefore silently unroutable — `allRemotes()` hands it
/// straight to that path — and no packet can ever match it either, since
/// the wire always carries the sender's real name. Same class as the two
/// above: an identity that cannot mean what the config says it means.
///
/// There is deliberately NO duplicate-identity check. Before #67 two
/// DIFFERENT labels could resolve to one identity through divergent
/// `remotes.<label>.name` overrides, which had to be rejected rather than
/// merged. With the label as the identity, two distinct `remotes_list`
/// entries are by construction two distinct identities; the only way an
/// identity can repeat is an exact repeat of the label, which names the
/// same parameter block and is handled below as the idempotent case it
/// always was.
///
/// @param labels `remotes_list` entries, in order.
/// @param local_name this bridge's own wire name, which no remote may
///        equal. Empty disables the check (for callers that have no
///        local name to compare against).
/// @param error set to a human-readable description when a label is
///        rejected; untouched otherwise. May be null.
/// @return the valid labels, repeats collapsed, in first-seen order; or an
///         empty vector on rejection.
inline std::vector<std::string> resolveRemoteIdentities(
  const std::vector<std::string>& labels,
  const std::string& local_name = std::string(),
  std::string* error = nullptr)
{
  std::vector<std::string> identities;
  std::set<std::string> seen;
  for(const auto& label: labels)
  {
    // A label repeated in `remotes_list` is not a collision: it names the
    // same parameter block, so it is the same remote twice and configuring
    // it twice was always idempotent. Skip it rather than reporting that a
    // remote collides with itself.
    if(seen.count(label))
      continue;
    if(label.empty())
    {
      if(error)
        *error = "a remotes_list entry is empty, so it names the empty"
                 " remote name. The empty name is reserved on the send path"
                 " (it marks a connection request), so such a remote can"
                 " never be routed to and no arriving packet can ever match"
                 " it. Give the entry a label — the label IS the name that"
                 " remote calls itself on the wire.";
      return std::vector<std::string>();
    }
    if(!node_name_fits(label))
    {
      if(error)
        *error = node_name_too_long_error(
          "remote label (remotes_list entry; it is the remote's wire name,"
          " so both bridges must be renamed together)",
          label);
      return std::vector<std::string>();
    }
    if(!local_name.empty() && label == local_name)
    {
      if(error)
        *error = "remote '" + label + "' has this bridge's own name. A"
                 " bridge cannot be its own remote: it would be installed"
                 " in remote_nodes_ under our name and the receive path"
                 " would then accept our own packets as a peer's. Change"
                 " either the `name` parameter or the remotes_list entry.";
      return std::vector<std::string>();
    }
    seen.insert(label);
    identities.push_back(label);
  }
  return identities;
}

} // namespace udp_bridge

#endif
