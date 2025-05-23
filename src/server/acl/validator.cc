// Copyright 2025, DragonflyDB authors.  All rights reserved.
// See LICENSE for licensing terms.
//

#include "server/acl/validator.h"

#include "base/logging.h"
#include "core/glob_matcher.h"
#include "facade/dragonfly_connection.h"
#include "server/acl/acl_commands_def.h"
#include "server/command_registry.h"
#include "server/server_state.h"
#include "server/transaction.h"

namespace dfly::acl {

namespace {

bool Matches(std::string_view pattern, std::string_view target) {
  GlobMatcher matcher(pattern, true);
  return matcher.Matches(target);
};

bool ValidateCommand(const std::vector<uint64_t>& acl_commands, const CommandId& id) {
  const size_t index = id.GetFamily();
  const uint64_t command_mask = id.GetBitIndex();
  DCHECK_LT(index, acl_commands.size());

  return (acl_commands[index] & command_mask) != 0;
}

[[nodiscard]] std::pair<bool, AclLog::Reason> IsPubSubCommandAuthorized(
    bool literal_match, const std::vector<uint64_t>& acl_commands, const AclPubSub& pub_sub,
    CmdArgList tail_args, const CommandId& id) {
  if (!ValidateCommand(acl_commands, id)) {
    return {false, AclLog::Reason::COMMAND};
  }

  auto iterate_globs = [&](std::string_view target) {
    for (auto& [glob, has_asterisk] : pub_sub.globs) {
      if (literal_match && (glob == target)) {
        return true;
      }
      if (!literal_match && Matches(glob, target)) {
        return true;
      }
    }
    return false;
  };

  bool allowed = true;
  if (!pub_sub.all_channels) {
    for (auto channel : tail_args) {
      allowed &= iterate_globs(facade::ToSV(channel));
    }
  }

  return {allowed, AclLog::Reason::PUB_SUB};
}

}  // namespace

[[nodiscard]] bool IsUserAllowedToInvokeCommand(const ConnectionContext& cntx, const CommandId& id,
                                                ArgSlice tail_args) {
  if (cntx.skip_acl_validation) {
    return true;
  }

  if (id.IsAlias()) {
    return false;
  }

  std::pair<bool, AclLog::Reason> auth_res;

  if (id.IsPubSub() || id.IsShardedPSub()) {
    auth_res = IsPubSubCommandAuthorized(false, cntx.acl_commands, cntx.pub_sub, tail_args, id);
  } else if (id.IsPSub()) {
    auth_res = IsPubSubCommandAuthorized(true, cntx.acl_commands, cntx.pub_sub, tail_args, id);
  } else {
    auth_res = IsUserAllowedToInvokeCommandGeneric(cntx, id, tail_args);
  }

  const auto [is_authed, reason] = auth_res;

  if (!is_authed) {
    auto& log = ServerState::tlocal()->acl_log;
    log.Add(cntx, std::string(id.name()), reason);
  }

  return is_authed;
}

[[nodiscard]] std::pair<bool, AclLog::Reason> IsUserAllowedToInvokeCommandGeneric(
    const ConnectionContext& cntx, const CommandId& id, CmdArgList tail_args) {
  const size_t max = std::numeric_limits<size_t>::max();
  // Once we support ranges this must change
  const bool reject_move_command = cntx.acl_db_idx != max && id.name() == "MOVE";
  const bool reject_trans_command =
      cntx.acl_db_idx != max && cntx.acl_db_idx != cntx.db_index() && id.IsTransactional();
  if (reject_move_command || reject_trans_command) {
    return {false, AclLog::Reason::AUTH};
  }
  size_t res = 0;
  if (tail_args.size() == 1 && id.name() == "SELECT" && absl::SimpleAtoi(tail_args[0], &res) &&
      cntx.acl_db_idx != max && cntx.acl_db_idx != res) {
    return {false, AclLog::Reason::AUTH};
  }

  const auto& acl_commands = cntx.acl_commands;
  const auto& keys = cntx.keys;
  if (!ValidateCommand(acl_commands, id)) {
    return {false, AclLog::Reason::COMMAND};
  }

  const bool is_read_command = id.IsReadOnly();
  const bool is_write_command = id.IsWriteOnly();

  auto iterate_globs = [&](auto target) {
    for (auto& [elem, op] : keys.key_globs) {
      if (Matches(elem, target)) {
        if (is_read_command && (op == KeyOp::READ || op == KeyOp::READ_WRITE)) {
          return true;
        }
        if (is_write_command && (op == KeyOp::WRITE || op == KeyOp::READ_WRITE)) {
          return true;
        }
      }
    }
    return false;
  };

  bool keys_allowed = true;
  if (!keys.all_keys && id.first_key_pos() != 0 && (is_read_command || is_write_command)) {
    auto keys_index = DetermineKeys(&id, tail_args);
    DCHECK(keys_index);

    for (std::string_view key : keys_index->Range(tail_args))
      keys_allowed &= iterate_globs(key);
  }

  return {keys_allowed, AclLog::Reason::KEY};
}

}  // namespace dfly::acl
