#pragma once
#include <string>

// Which wallet rail a spend recipient selects, decided purely by the recipient's FORM. The
// form is what the spend_requests row stores, so walletSend, approveSpend and the A2A payout
// path all route the same spend identically without a schema change:
//   keys JSON {nullifier_public_key, viewing_public_key} -> PrivateKeys  (transfer_private)
//   "public:<64 hex>"                                     -> Public       (transfer_public from
//                                                            the agent's funded public account)
//   anything else                                         -> PrivateOwned (transfer_private_owned)
//   "public:" with a malformed id                         -> Invalid      (never reaches the wallet)
// A public account id and a private account id are both 32-byte hex, so the public rail
// needs the explicit prefix — shape alone cannot tell them apart.
enum class SpendRail { PrivateKeys, PrivateOwned, Public, Invalid };

struct SpendTarget {
    SpendRail rail;
    std::string target;   // what the wallet receives: the keys JSON, or a lower-case 64-hex id
};

SpendTarget parseSpendRecipient(const std::string& recipient);
