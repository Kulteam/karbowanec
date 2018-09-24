// Copyright (c) 2012-2016, The CryptoNote developers, The Bytecoin developers
// Copyright (c) 2016, The Forknote developers
// Copyright (c) 2016-2018, The Karbowanec developers
//
// This file is part of Bytecoin.
//
// Bytecoin is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// Bytecoin is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with Bytecoin.  If not, see <http://www.gnu.org/licenses/>.

#include "RpcServer.h"
#include "version.h"

#include <future>
#include <unordered_map>

// CryptoNote
#include "Common/StringTools.h"
#include "Common/Base58.h"
#include "CryptoNoteCore/TransactionUtils.h"
#include "CryptoNoteCore/CryptoNoteTools.h"
#include "CryptoNoteCore/CryptoNoteFormatUtils.h"
#include "CryptoNoteCore/Core.h"
#include "CryptoNoteCore/IBlock.h"
#include "CryptoNoteCore/Miner.h"
#include "CryptoNoteCore/TransactionExtra.h"
#include "CryptoNoteProtocol/ICryptoNoteProtocolQuery.h"

#include "P2p/NetNode.h"

#include "CoreRpcServerErrorCodes.h"
#include "JsonRpc.h"

#undef ERROR

using namespace Logging;
using namespace Crypto;
using namespace Common;

namespace CryptoNote {

namespace {

template <typename Command>
RpcServer::HandlerFunction binMethod(bool (RpcServer::*handler)(typename Command::request const&, typename Command::response&)) {
  return [handler](RpcServer* obj, const HttpRequest& request, HttpResponse& response) {

    boost::value_initialized<typename Command::request> req;
    boost::value_initialized<typename Command::response> res;

    if (!loadFromBinaryKeyValue(static_cast<typename Command::request&>(req), request.getBody())) {
      return false;
    }

    bool result = (obj->*handler)(req, res);
    response.setBody(storeToBinaryKeyValue(res.data()));
    return result;
  };
}

template <typename Command>
RpcServer::HandlerFunction jsonMethod(bool (RpcServer::*handler)(typename Command::request const&, typename Command::response&)) {
  return [handler](RpcServer* obj, const HttpRequest& request, HttpResponse& response) {

    boost::value_initialized<typename Command::request> req;
    boost::value_initialized<typename Command::response> res;

    if (!loadFromJson(static_cast<typename Command::request&>(req), request.getBody())) {
      return false;
    }

    bool result = (obj->*handler)(req, res);
    response.setBody(storeToJson(res.data()));
    return result;
  };
}

}
  
std::unordered_map<std::string, RpcServer::RpcHandler<RpcServer::HandlerFunction>> RpcServer::s_handlers = {
  
  // binary handlers
  { "/getblocks.bin", { binMethod<COMMAND_RPC_GET_BLOCKS_FAST>(&RpcServer::on_get_blocks), false } },
  { "/queryblocks.bin", { binMethod<COMMAND_RPC_QUERY_BLOCKS>(&RpcServer::on_query_blocks), false } },
  { "/queryblockslite.bin", { binMethod<COMMAND_RPC_QUERY_BLOCKS_LITE>(&RpcServer::on_query_blocks_lite), false } },
  { "/get_o_indexes.bin", { binMethod<COMMAND_RPC_GET_TX_GLOBAL_OUTPUTS_INDEXES>(&RpcServer::on_get_indexes), false } },
  { "/getrandom_outs.bin", { binMethod<COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS>(&RpcServer::on_get_random_outs), false } },
  { "/get_pool_changes.bin", { binMethod<COMMAND_RPC_GET_POOL_CHANGES>(&RpcServer::onGetPoolChanges), false } },
  { "/get_pool_changes_lite.bin", { binMethod<COMMAND_RPC_GET_POOL_CHANGES_LITE>(&RpcServer::onGetPoolChangesLite), false } },

  // json handlers
  { "/getinfo", { jsonMethod<COMMAND_RPC_GET_INFO>(&RpcServer::on_get_info), true } },
  { "/getheight", { jsonMethod<COMMAND_RPC_GET_HEIGHT>(&RpcServer::on_get_height), true } },
  { "/gettransactions", { jsonMethod<COMMAND_RPC_GET_TRANSACTIONS>(&RpcServer::on_get_transactions), false } },
  { "/sendrawtransaction", { jsonMethod<COMMAND_RPC_SEND_RAW_TX>(&RpcServer::on_send_raw_tx), false } },
  { "/feeaddress", { jsonMethod<COMMAND_RPC_GET_FEE_ADDRESS>(&RpcServer::on_get_fee_address), true } },
  { "/peers", { jsonMethod<COMMAND_RPC_GET_PEER_LIST>(&RpcServer::on_get_peer_list), true } },
  { "/paymentid", { jsonMethod<COMMAND_RPC_GEN_PAYMENT_ID>(&RpcServer::on_get_payment_id), true } },
  
  // disabled in restricted rpc mode
  { "/start_mining", { jsonMethod<COMMAND_RPC_START_MINING>(&RpcServer::on_start_mining), false } },
  { "/stop_mining", { jsonMethod<COMMAND_RPC_STOP_MINING>(&RpcServer::on_stop_mining), false } },
  { "/stop_daemon", { jsonMethod<COMMAND_RPC_STOP_DAEMON>(&RpcServer::on_stop_daemon), true } },

  // json rpc
  { "/json_rpc", { std::bind(&RpcServer::processJsonRpcRequest, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3), true } }
};

RpcServer::RpcServer(System::Dispatcher& dispatcher, Logging::ILogger& log, core& c, NodeServer& p2p, const ICryptoNoteProtocolQuery& protocolQuery) :
  HttpServer(dispatcher, log), logger(log, "RpcServer"), m_core(c), m_p2p(p2p), m_protocolQuery(protocolQuery) {
}

void RpcServer::processRequest(const HttpRequest& request, HttpResponse& response) {
  logger(TRACE) << "RPC request came: \n" << request << std::endl;

  auto url = request.getUrl();

  auto it = s_handlers.find(url);
  if (it == s_handlers.end()) {
    response.setStatus(HttpResponse::STATUS_404);
    return;
  }

  if (!it->second.allowBusyCore && !isCoreReady()) {
    response.setStatus(HttpResponse::STATUS_500);
    response.setBody("Core is busy");
    return;
  }

  it->second.handler(this, request, response);
}

bool RpcServer::processJsonRpcRequest(const HttpRequest& request, HttpResponse& response) {

  using namespace JsonRpc;

  response.addHeader("Content-Type", "application/json");
  if (!m_cors_domain.empty()) {
	response.addHeader("Access-Control-Allow-Origin", m_cors_domain);
  }	

  JsonRpcRequest jsonRequest;
  JsonRpcResponse jsonResponse;

  try {
    logger(TRACE) << "JSON-RPC request: " << request.getBody();
    jsonRequest.parseRequest(request.getBody());
    jsonResponse.setId(jsonRequest.getId()); // copy id

    static std::unordered_map<std::string, RpcServer::RpcHandler<JsonMemberMethod>> jsonRpcHandlers = {
	
      { "getblockcount", { makeMemberMethod(&RpcServer::on_getblockcount), true } },
      { "on_getblockhash", { makeMemberMethod(&RpcServer::on_getblockhash), false } },
      { "getblocktemplate", { makeMemberMethod(&RpcServer::on_getblocktemplate), false } },
      { "getcurrencyid", { makeMemberMethod(&RpcServer::on_get_currency_id), true } },
      { "submitblock", { makeMemberMethod(&RpcServer::on_submitblock), false } },
      { "getlastblockheader", { makeMemberMethod(&RpcServer::on_get_last_block_header), false } },
      { "getblockheaderbyhash", { makeMemberMethod(&RpcServer::on_get_block_header_by_hash), false } },
      { "getblockheaderbyheight", { makeMemberMethod(&RpcServer::on_get_block_header_by_height), false } },
	  { "f_blocks_list_json", { makeMemberMethod(&RpcServer::f_on_blocks_list_json), false } },
      { "f_block_json", { makeMemberMethod(&RpcServer::f_on_block_json), false } },
      { "f_transaction_json", { makeMemberMethod(&RpcServer::f_on_transaction_json), false } },
	  { "f_pool_json", { makeMemberMethod(&RpcServer::f_on_pool_json), false } },
	  { "f_mempool_json", { makeMemberMethod(&RpcServer::f_on_mempool_json), false } },
	  { "k_transactions_by_payment_id", { makeMemberMethod(&RpcServer::k_on_transactions_by_payment_id), false } },
	  { "get_transaction_hashes_by_payment_id", { makeMemberMethod(&RpcServer::onGetTransactionHashesByPaymentId), false } },
	  { "check_tx_key", { makeMemberMethod(&RpcServer::k_on_check_tx_key), false } },
	  { "check_tx_with_view_key", { makeMemberMethod(&RpcServer::k_on_check_tx_with_view_key), false } },
	  { "validateaddress", { makeMemberMethod(&RpcServer::on_validate_address), false } },
	  { "verifymessage", { makeMemberMethod(&RpcServer::on_verify_message), false } }

    };

    auto it = jsonRpcHandlers.find(jsonRequest.getMethod());
    if (it == jsonRpcHandlers.end()) {
      throw JsonRpcError(JsonRpc::errMethodNotFound);
    }

    if (!it->second.allowBusyCore && !isCoreReady()) {
      throw JsonRpcError(CORE_RPC_ERROR_CODE_CORE_BUSY, "Core is busy");
    }

    it->second.handler(this, jsonRequest, jsonResponse);

  } catch (const JsonRpcError& err) {
    jsonResponse.setError(err);
  } catch (const std::exception& e) {
    jsonResponse.setError(JsonRpcError(JsonRpc::errInternalError, e.what()));
  }

  response.setBody(jsonResponse.getBody());
  logger(TRACE) << "JSON-RPC response: " << jsonResponse.getBody();
  return true;
}

bool RpcServer::restrictRPC(const bool is_restricted) {
  m_restricted_rpc = is_restricted;
  return true;
}

bool RpcServer::enableCors(const std::string domain) {
  m_cors_domain = domain;
  return true;
}

bool RpcServer::setFeeAddress(const std::string& fee_address, const AccountPublicAddress& fee_acc) {
  m_fee_address = fee_address;
  m_fee_acc = fee_acc;
  return true;
}

bool RpcServer::setViewKey(const std::string& view_key) {
  Crypto::Hash private_view_key_hash;
  size_t size;
  if (!Common::fromHex(view_key, &private_view_key_hash, sizeof(private_view_key_hash), size) || size != sizeof(private_view_key_hash)) {
    logger(INFO) << "Could not parse private view key";
    return false;
  }
  m_view_key = *(struct Crypto::SecretKey *) &private_view_key_hash;
  return true;
}

bool RpcServer::isCoreReady() {
  return m_core.currency().isTestnet() || m_p2p.get_payload_object().isSynchronized();
}

bool RpcServer::masternode_check_incoming_tx(const BinaryArray& tx_blob) {
	Crypto::Hash tx_hash = NULL_HASH;
	Crypto::Hash tx_prefixt_hash = NULL_HASH;
	Transaction tx;
	if (!parseAndValidateTransactionFromBinaryArray(tx_blob, tx, tx_hash, tx_prefixt_hash)) {
		logger(INFO) << "Could not parse tx from blob";
		return false;
	}
	CryptoNote::TransactionPrefix transaction = *static_cast<const TransactionPrefix*>(&tx);

	std::vector<uint32_t> out;
	uint64_t amount;

	if (!CryptoNote::findOutputsToAccount(transaction, m_fee_acc, m_view_key, out, amount)) {
		logger(INFO) << "Could not find outputs to masternode fee address";
		return false;
	}

	if (amount != 0) {
		logger(INFO) << "Masternode received relayed transaction fee: " << m_core.currency().formatAmount(amount) << " KRB";
		return true;
	}
	return false;
}

//
// Binary handlers
//

bool RpcServer::on_get_blocks(const COMMAND_RPC_GET_BLOCKS_FAST::request& req, COMMAND_RPC_GET_BLOCKS_FAST::response& res) {
  // TODO code duplication see InProcessNode::doGetNewBlocks()
  if (req.block_ids.empty()) {
    res.status = "Failed";
    return false;
  }

  if (req.block_ids.back() != m_core.getBlockIdByHeight(0)) {
    res.status = "Failed";
    return false;
  }

  uint32_t totalBlockCount;
  uint32_t startBlockIndex;
  std::vector<Crypto::Hash> supplement = m_core.findBlockchainSupplement(req.block_ids, COMMAND_RPC_GET_BLOCKS_FAST_MAX_COUNT, totalBlockCount, startBlockIndex);

  res.current_height = totalBlockCount;
  res.start_height = startBlockIndex;

  for (const auto& blockId : supplement) {
    assert(m_core.have_block(blockId));
    auto completeBlock = m_core.getBlock(blockId);
    assert(completeBlock != nullptr);

    res.blocks.resize(res.blocks.size() + 1);
    res.blocks.back().block = asString(toBinaryArray(completeBlock->getBlock()));

    res.blocks.back().txs.reserve(completeBlock->getTransactionCount());
    for (size_t i = 0; i < completeBlock->getTransactionCount(); ++i) {
      res.blocks.back().txs.push_back(asString(toBinaryArray(completeBlock->getTransaction(i))));
    }
  }

  res.status = CORE_RPC_STATUS_OK;
  return true;
}

bool RpcServer::on_query_blocks(const COMMAND_RPC_QUERY_BLOCKS::request& req, COMMAND_RPC_QUERY_BLOCKS::response& res) {
  uint32_t startHeight;
  uint32_t currentHeight;
  uint32_t fullOffset;

  if (!m_core.queryBlocks(req.block_ids, req.timestamp, startHeight, currentHeight, fullOffset, res.items)) {
    res.status = "Failed to perform query";
    return false;
  }

  res.start_height = startHeight;
  res.current_height = currentHeight;
  res.full_offset = fullOffset;
  res.status = CORE_RPC_STATUS_OK;
  return true;
}

bool RpcServer::on_query_blocks_lite(const COMMAND_RPC_QUERY_BLOCKS_LITE::request& req, COMMAND_RPC_QUERY_BLOCKS_LITE::response& res) {
  uint32_t startHeight;
  uint32_t currentHeight;
  uint32_t fullOffset;
  if (!m_core.queryBlocksLite(req.blockIds, req.timestamp, startHeight, currentHeight, fullOffset, res.items)) {
    res.status = "Failed to perform query";
    return false;
  }

  res.startHeight = startHeight;
  res.currentHeight = currentHeight;
  res.fullOffset = fullOffset;
  res.status = CORE_RPC_STATUS_OK;
  return true;
}

bool RpcServer::on_get_indexes(const COMMAND_RPC_GET_TX_GLOBAL_OUTPUTS_INDEXES::request& req, COMMAND_RPC_GET_TX_GLOBAL_OUTPUTS_INDEXES::response& res) {
  std::vector<uint32_t> outputIndexes;
  if (!m_core.get_tx_outputs_gindexs(req.txid, outputIndexes)) {
    res.status = "Failed";
    return true;
  }

  res.o_indexes.assign(outputIndexes.begin(), outputIndexes.end());
  res.status = CORE_RPC_STATUS_OK;
  logger(TRACE) << "COMMAND_RPC_GET_TX_GLOBAL_OUTPUTS_INDEXES: [" << res.o_indexes.size() << "]";
  return true;
}

bool RpcServer::on_get_random_outs(const COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::request& req, COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::response& res) {
  res.status = "Failed";
  if (!m_core.get_random_outs_for_amounts(req, res)) {
    return true;
  }

  res.status = CORE_RPC_STATUS_OK;

  std::stringstream ss;
  typedef COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::outs_for_amount outs_for_amount;
  typedef COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::out_entry out_entry;

  std::for_each(res.outs.begin(), res.outs.end(), [&](outs_for_amount& ofa)  {
    ss << "[" << ofa.amount << "]:";

    assert(ofa.outs.size() && "internal error: ofa.outs.size() is empty");

    std::for_each(ofa.outs.begin(), ofa.outs.end(), [&](out_entry& oe)
    {
      ss << oe.global_amount_index << " ";
    });
    ss << ENDL;
  });
  std::string s = ss.str();
  logger(TRACE) << "COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS: " << ENDL << s;
  res.status = CORE_RPC_STATUS_OK;
  return true;
}

bool RpcServer::onGetPoolChanges(const COMMAND_RPC_GET_POOL_CHANGES::request& req, COMMAND_RPC_GET_POOL_CHANGES::response& rsp) {
  rsp.status = CORE_RPC_STATUS_OK;
  std::vector<CryptoNote::Transaction> addedTransactions;
  rsp.isTailBlockActual = m_core.getPoolChanges(req.tailBlockId, req.knownTxsIds, addedTransactions, rsp.deletedTxsIds);
  for (auto& tx : addedTransactions) {
    BinaryArray txBlob;
    if (!toBinaryArray(tx, txBlob)) {
      rsp.status = "Internal error";
      break;;
    }

    rsp.addedTxs.emplace_back(std::move(txBlob));
  }
  return true;
}


bool RpcServer::onGetPoolChangesLite(const COMMAND_RPC_GET_POOL_CHANGES_LITE::request& req, COMMAND_RPC_GET_POOL_CHANGES_LITE::response& rsp) {
  rsp.status = CORE_RPC_STATUS_OK;
  rsp.isTailBlockActual = m_core.getPoolChangesLite(req.tailBlockId, req.knownTxsIds, rsp.addedTxs, rsp.deletedTxsIds);

  return true;
}

//
// JSON handlers
//

bool RpcServer::on_get_info(const COMMAND_RPC_GET_INFO::request& req, COMMAND_RPC_GET_INFO::response& res) {
  res.height = m_core.get_current_blockchain_height();
  res.difficulty = m_core.getNextBlockDifficulty();
  res.tx_count = m_core.get_blockchain_total_transactions() - res.height; //without coinbase
  res.tx_pool_size = m_core.get_pool_transactions_count();
  res.alt_blocks_count = m_core.get_alternative_blocks_count();
  uint64_t total_conn = m_p2p.get_connections_count();
  res.outgoing_connections_count = m_p2p.get_outgoing_connections_count();
  res.incoming_connections_count = total_conn - res.outgoing_connections_count;
  res.rpc_connections_count = get_connections_count();
  res.white_peerlist_size = m_p2p.getPeerlistManager().get_white_peers_count();
  res.grey_peerlist_size = m_p2p.getPeerlistManager().get_gray_peers_count();
  res.last_known_block_index = std::max(static_cast<uint32_t>(1), m_protocolQuery.getObservedHeight()) - 1;
  res.top_block_hash = Common::podToHex(m_core.getBlockIdByHeight(m_core.get_current_blockchain_height() - 1));
  res.version = PROJECT_VERSION_LONG;
  res.fee_address = m_fee_address.empty() ? std::string() : m_fee_address;
  res.min_tx_fee = m_core.getMinimalFee();
  res.readable_tx_fee = m_core.currency().formatAmount(m_core.getMinimalFee());
  res.start_time = (uint64_t)m_core.getStartTime();
  res.block_major_version = m_core.getCurrentBlockMajorVersion();
  // that large uint64_t number is unsafe in JavaScript environment and therefore as a JSON value so we display it as a formatted string
  res.already_generated_coins = m_core.currency().formatAmount(m_core.getTotalGeneratedAmount());

  res.status = CORE_RPC_STATUS_OK;
  return true;
}

bool RpcServer::on_get_height(const COMMAND_RPC_GET_HEIGHT::request& req, COMMAND_RPC_GET_HEIGHT::response& res) {
  res.height = m_core.get_current_blockchain_height();
  res.status = CORE_RPC_STATUS_OK;
  return true;
}

bool RpcServer::on_get_transactions(const COMMAND_RPC_GET_TRANSACTIONS::request& req, COMMAND_RPC_GET_TRANSACTIONS::response& res) {
  std::vector<Hash> vh;
  for (const auto& tx_hex_str : req.txs_hashes) {
    BinaryArray b;
    if (!fromHex(tx_hex_str, b))
    {
      res.status = "Failed to parse hex representation of transaction hash";
      return true;
    }
    if (b.size() != sizeof(Hash))
    {
      res.status = "Failed, size of data mismatch";
    }
    vh.push_back(*reinterpret_cast<const Hash*>(b.data()));
  }
  std::list<Hash> missed_txs;
  std::list<Transaction> txs;
  m_core.getTransactions(vh, txs, missed_txs);

  for (auto& tx : txs) {
    res.txs_as_hex.push_back(toHex(toBinaryArray(tx)));
  }

  for (const auto& miss_tx : missed_txs) {
    res.missed_tx.push_back(Common::podToHex(miss_tx));
  }

  res.status = CORE_RPC_STATUS_OK;
  return true;
}

bool RpcServer::on_send_raw_tx(const COMMAND_RPC_SEND_RAW_TX::request& req, COMMAND_RPC_SEND_RAW_TX::response& res) {
  BinaryArray tx_blob;
  if (!fromHex(req.tx_as_hex, tx_blob))
  {
    logger(INFO) << "[on_send_raw_tx]: Failed to parse tx from hexbuff: " << req.tx_as_hex;
    res.status = "Failed";
    return true;
  }

  Crypto::Hash transactionHash = Crypto::cn_fast_hash(tx_blob.data(), tx_blob.size());
  logger(DEBUGGING) << "transaction " << transactionHash << " came in on_send_raw_tx";

  tx_verification_context tvc = boost::value_initialized<tx_verification_context>();
  if (!m_core.handle_incoming_tx(tx_blob, tvc, false, false))
  {
    logger(INFO) << "[on_send_raw_tx]: Failed to process tx";
    res.status = "Failed";
    return true;
  }

  if (tvc.m_verification_failed)
  {
    logger(INFO) << "[on_send_raw_tx]: tx verification failed";
    res.status = "Failed";
    return true;
  }

  if (!tvc.m_should_be_relayed)
  {
    logger(INFO) << "[on_send_raw_tx]: tx accepted, but not relayed";
    res.status = "Not relayed";
    return true;
  }

  if (!m_fee_address.empty() && m_view_key != NULL_SECRET_KEY) {
	if (!masternode_check_incoming_tx(tx_blob)) {
	  logger(INFO) << "Transaction not relayed due to lack of masternode fee";		
      res.status = "Not relayed due to lack of node fee";
      return true;
	}
  }

  NOTIFY_NEW_TRANSACTIONS::request r;
  r.txs.push_back(asString(tx_blob));
  m_core.get_protocol()->relay_transactions(r);
  //TODO: make sure that tx has reached other nodes here, probably wait to receive reflections from other nodes
  res.status = CORE_RPC_STATUS_OK;
  return true;
}

bool RpcServer::on_start_mining(const COMMAND_RPC_START_MINING::request& req, COMMAND_RPC_START_MINING::response& res) {
  if (m_restricted_rpc) {
	res.status = "Failed, restricted handle";
	return false;
  }
  
  AccountPublicAddress adr;
  if (!m_core.currency().parseAccountAddressString(req.miner_address, adr)) {
    res.status = "Failed, wrong address";
    return true;
  }

  if (!m_core.get_miner().start(adr, static_cast<size_t>(req.threads_count))) {
    res.status = "Failed, mining not started";
    return true;
  }

  res.status = CORE_RPC_STATUS_OK;
  return true;
}

bool RpcServer::on_stop_mining(const COMMAND_RPC_STOP_MINING::request& req, COMMAND_RPC_STOP_MINING::response& res) {
  if (m_restricted_rpc) {
	res.status = "Failed, restricted handle";
	return false;
  }

  if (!m_core.get_miner().stop()) {
    res.status = "Failed, mining not stopped";
    return true;
  }
  res.status = CORE_RPC_STATUS_OK;
  return true;
}

bool RpcServer::on_stop_daemon(const COMMAND_RPC_STOP_DAEMON::request& req, COMMAND_RPC_STOP_DAEMON::response& res) {
  if (m_restricted_rpc) {
	res.status = "Failed, restricted handle";
	return false;
  }
  if (m_core.currency().isTestnet()) {
    m_p2p.sendStopSignal();
    res.status = CORE_RPC_STATUS_OK;
  } else {
    res.status = CORE_RPC_ERROR_CODE_INTERNAL_ERROR;
    return false;
  }
  return true;
}

bool RpcServer::on_get_fee_address(const COMMAND_RPC_GET_FEE_ADDRESS::request& req, COMMAND_RPC_GET_FEE_ADDRESS::response& res) {
  if (m_fee_address.empty()) {
	res.status = CORE_RPC_STATUS_OK;
	return false; 
  }
  res.fee_address = m_fee_address;
  res.status = CORE_RPC_STATUS_OK;
  return true;
}

bool RpcServer::on_get_peer_list(const COMMAND_RPC_GET_PEER_LIST::request& req, COMMAND_RPC_GET_PEER_LIST::response& res) {
	std::list<PeerlistEntry> pl_wite;
	std::list<PeerlistEntry> pl_gray;
	m_p2p.getPeerlistManager().get_peerlist_full(pl_gray, pl_wite);
	for (const auto& pe : pl_wite) {
		std::stringstream ss;
		ss << pe.adr;
		res.peers.push_back(ss.str());
	}
	res.status = CORE_RPC_STATUS_OK;
	return true;
}

bool RpcServer::on_get_payment_id(const COMMAND_RPC_GEN_PAYMENT_ID::request& req, COMMAND_RPC_GEN_PAYMENT_ID::response& res) {
  std::string pid;
  try {
    pid = Common::podToHex(Crypto::rand<Crypto::Hash>());
  } catch (const std::exception& e) {
    throw JsonRpc::JsonRpcError{ CORE_RPC_ERROR_CODE_INTERNAL_ERROR, "Internal error: can't generate Payment ID" };
  }
  res.payment_id = pid;
  return true;
}

//------------------------------------------------------------------------------------------------------------------------------
// JSON RPC methods
//------------------------------------------------------------------------------------------------------------------------------

bool RpcServer::f_on_blocks_list_json(const F_COMMAND_RPC_GET_BLOCKS_LIST::request& req, F_COMMAND_RPC_GET_BLOCKS_LIST::response& res) {
  if (m_core.get_current_blockchain_height() <= req.height) {
    throw JsonRpc::JsonRpcError{ CORE_RPC_ERROR_CODE_TOO_BIG_HEIGHT,
      std::string("To big height: ") + std::to_string(req.height) + ", current blockchain height = " + std::to_string(m_core.get_current_blockchain_height()) };
  }

  uint32_t print_blocks_count = 30;
  uint32_t last_height = req.height - print_blocks_count;
  if (req.height <= print_blocks_count)  {
    last_height = 0;
  } 

  for (uint32_t i = req.height; i >= last_height; i--) {
    Hash block_hash = m_core.getBlockIdByHeight(static_cast<uint32_t>(i));
    Block blk;
    if (!m_core.getBlockByHash(block_hash, blk)) {
      throw JsonRpc::JsonRpcError{ CORE_RPC_ERROR_CODE_INTERNAL_ERROR,
        "Internal error: can't get block by height. Height = " + std::to_string(i) + '.' };
    }

    size_t tx_cumulative_block_size;
    m_core.getBlockSize(block_hash, tx_cumulative_block_size);
    size_t blokBlobSize = getObjectBinarySize(blk);
    size_t minerTxBlobSize = getObjectBinarySize(blk.baseTransaction);
    difficulty_type blockDiff;
    m_core.getBlockDifficulty(static_cast<uint32_t>(i), blockDiff);

    f_block_short_response block_short;
    block_short.timestamp = blk.timestamp;
    block_short.height = i;
    block_short.hash = Common::podToHex(block_hash);
    block_short.cumul_size = blokBlobSize + tx_cumulative_block_size - minerTxBlobSize;
    block_short.tx_count = blk.transactionHashes.size() + 1;
	block_short.difficulty = blockDiff;
	block_short.min_tx_fee = m_core.getMinimalFeeForHeight(i);

    res.blocks.push_back(block_short);

    if (i == 0)
      break;
  }

  res.status = CORE_RPC_STATUS_OK;
  return true;
}

bool RpcServer::f_on_block_json(const F_COMMAND_RPC_GET_BLOCK_DETAILS::request& req, F_COMMAND_RPC_GET_BLOCK_DETAILS::response& res) {
  Hash hash;

  try {
    uint32_t height = boost::lexical_cast<uint32_t>(req.hash);
    hash = m_core.getBlockIdByHeight(height);
  } catch (boost::bad_lexical_cast &) {
    if (!parse_hash256(req.hash, hash)) {
      throw JsonRpc::JsonRpcError{
        CORE_RPC_ERROR_CODE_WRONG_PARAM,
        "Failed to parse hex representation of block hash. Hex = " + req.hash + '.' };
    }
  }

  Block blk;
  if (!m_core.getBlockByHash(hash, blk)) {
    throw JsonRpc::JsonRpcError{
      CORE_RPC_ERROR_CODE_INTERNAL_ERROR,
      "Internal error: can't get block by hash. Hash = " + req.hash + '.' };
  }

  if (blk.baseTransaction.inputs.front().type() != typeid(BaseInput)) {
    throw JsonRpc::JsonRpcError{
      CORE_RPC_ERROR_CODE_INTERNAL_ERROR,
      "Internal error: coinbase transaction in the block has the wrong type" };
  }

  block_header_response block_header;
  res.block.height = boost::get<BaseInput>(blk.baseTransaction.inputs.front()).blockIndex;

  Crypto::Hash tmp_hash = m_core.getBlockIdByHeight(res.block.height);
  bool is_orphaned = hash != tmp_hash;
  fill_block_header_response(blk, is_orphaned, res.block.height, hash, block_header);

  res.block.major_version = block_header.major_version;
  res.block.minor_version = block_header.minor_version;
  res.block.timestamp = block_header.timestamp;
  res.block.prev_hash = block_header.prev_hash;
  res.block.nonce = block_header.nonce;
  res.block.hash = block_header.hash;
  res.block.depth = block_header.depth;
  m_core.getBlockDifficulty(static_cast<uint32_t>(res.block.height), res.block.difficulty);

  res.block.reward = block_header.reward;

  std::vector<size_t> blocksSizes;
  if (!m_core.getBackwardBlocksSizes(res.block.height, blocksSizes, parameters::CRYPTONOTE_REWARD_BLOCKS_WINDOW)) {
    return false;
  }
  res.block.sizeMedian = Common::medianValue(blocksSizes);

  size_t blockSize = 0;
  if (!m_core.getBlockSize(hash, blockSize)) {
    return false;
  }
  res.block.transactionsCumulativeSize = blockSize;

  size_t blokBlobSize = getObjectBinarySize(blk);
  size_t minerTxBlobSize = getObjectBinarySize(blk.baseTransaction);
  res.block.blockSize = blokBlobSize + res.block.transactionsCumulativeSize - minerTxBlobSize;

  uint64_t alreadyGeneratedCoins;
  if (!m_core.getAlreadyGeneratedCoins(hash, alreadyGeneratedCoins)) {
    return false;
  }
  res.block.alreadyGeneratedCoins = std::to_string(alreadyGeneratedCoins);

  if (!m_core.getGeneratedTransactionsNumber(res.block.height, res.block.alreadyGeneratedTransactions)) {
    return false;
  }

  uint64_t prevBlockGeneratedCoins = 0;
  if (res.block.height > 0) {
    if (!m_core.getAlreadyGeneratedCoins(blk.previousBlockHash, prevBlockGeneratedCoins)) {
      return false;
    }
  }
  uint64_t maxReward = 0;
  uint64_t currentReward = 0;
  int64_t emissionChange = 0;
  size_t blockGrantedFullRewardZone =  CryptoNote::parameters::CRYPTONOTE_BLOCK_GRANTED_FULL_REWARD_ZONE;
  res.block.effectiveSizeMedian = std::max(res.block.sizeMedian, blockGrantedFullRewardZone);

  if (!m_core.getBlockReward(res.block.major_version, res.block.sizeMedian, 0, prevBlockGeneratedCoins, 0, maxReward, emissionChange)) {
    return false;
  }
  if (!m_core.getBlockReward(res.block.major_version, res.block.sizeMedian, res.block.transactionsCumulativeSize, prevBlockGeneratedCoins, 0, currentReward, emissionChange)) {
    return false;
  }

  res.block.baseReward = maxReward;
  if (maxReward == 0 && currentReward == 0) {
    res.block.penalty = static_cast<double>(0);
  } else {
    if (maxReward < currentReward) {
      return false;
    }
    res.block.penalty = static_cast<double>(maxReward - currentReward) / static_cast<double>(maxReward);
  }

  // Base transaction adding
  f_transaction_short_response transaction_short;
  transaction_short.hash = Common::podToHex(getObjectHash(blk.baseTransaction));
  transaction_short.fee = 0;
  transaction_short.amount_out = get_outs_money_amount(blk.baseTransaction);
  transaction_short.size = getObjectBinarySize(blk.baseTransaction);
  res.block.transactions.push_back(transaction_short);


  std::list<Crypto::Hash> missed_txs;
  std::list<Transaction> txs;
  m_core.getTransactions(blk.transactionHashes, txs, missed_txs);

  res.block.totalFeeAmount = 0;

  for (const Transaction& tx : txs) {
    f_transaction_short_response transaction_short;
    uint64_t amount_in = 0;
    get_inputs_money_amount(tx, amount_in);
    uint64_t amount_out = get_outs_money_amount(tx);

    transaction_short.hash = Common::podToHex(getObjectHash(tx));
    transaction_short.fee = amount_in - amount_out;
    transaction_short.amount_out = amount_out;
    transaction_short.size = getObjectBinarySize(tx);
    res.block.transactions.push_back(transaction_short);

    res.block.totalFeeAmount += transaction_short.fee;
  }

  res.status = CORE_RPC_STATUS_OK;
  return true;
}

bool RpcServer::f_on_transaction_json(const F_COMMAND_RPC_GET_TRANSACTION_DETAILS::request& req, F_COMMAND_RPC_GET_TRANSACTION_DETAILS::response& res) {
  Hash hash;

  if (!parse_hash256(req.hash, hash)) {
    throw JsonRpc::JsonRpcError{
      CORE_RPC_ERROR_CODE_WRONG_PARAM,
      "Failed to parse hex representation of transaction hash. Hex = " + req.hash + '.' };
  }

  std::vector<Crypto::Hash> tx_ids;
  tx_ids.push_back(hash);

  std::list<Crypto::Hash> missed_txs;
  std::list<Transaction> txs;
  m_core.getTransactions(tx_ids, txs, missed_txs, true);

  if (1 == txs.size()) {
    res.tx = txs.front();
  } else {
    throw JsonRpc::JsonRpcError{
      CORE_RPC_ERROR_CODE_WRONG_PARAM,
      "transaction wasn't found. Hash = " + req.hash + '.' };
  }

  Crypto::Hash blockHash;
  uint32_t blockHeight;
  if (m_core.getBlockContainingTx(hash, blockHash, blockHeight)) {
    Block blk;
    if (m_core.getBlockByHash(blockHash, blk)) {
      size_t tx_cumulative_block_size;
      m_core.getBlockSize(blockHash, tx_cumulative_block_size);
      size_t blokBlobSize = getObjectBinarySize(blk);
      size_t minerTxBlobSize = getObjectBinarySize(blk.baseTransaction);
      f_block_short_response block_short;

      block_short.timestamp = blk.timestamp;
      block_short.height = blockHeight;
      block_short.hash = Common::podToHex(blockHash);
      block_short.cumul_size = blokBlobSize + tx_cumulative_block_size - minerTxBlobSize;
      block_short.tx_count = blk.transactionHashes.size() + 1;
      res.block = block_short;
	  res.txDetails.confirmations = m_protocolQuery.getObservedHeight() - blockHeight;
    }
  }

  uint64_t amount_in = 0;
  get_inputs_money_amount(res.tx, amount_in);
  uint64_t amount_out = get_outs_money_amount(res.tx);

  res.txDetails.hash = Common::podToHex(getObjectHash(res.tx));
  res.txDetails.fee = amount_in - amount_out;
  if (amount_in == 0)
    res.txDetails.fee = 0;
  res.txDetails.amount_out = amount_out;
  res.txDetails.size = getObjectBinarySize(res.tx);

  uint64_t mixin;
  if (!f_getMixin(res.tx, mixin)) {
    return false;
  }
  res.txDetails.mixin = mixin;

  Crypto::Hash paymentId;
  if (CryptoNote::getPaymentIdFromTxExtra(res.tx.extra, paymentId)) {
    res.txDetails.paymentId = Common::podToHex(paymentId);
  } else {
    res.txDetails.paymentId = "";
  }

  res.status = CORE_RPC_STATUS_OK;
  return true;
}

bool RpcServer::f_on_pool_json(const F_COMMAND_RPC_GET_POOL::request& req, F_COMMAND_RPC_GET_POOL::response& res) {
  auto pool = m_core.getPoolTransactions();
  for (const Transaction tx : pool) {
    f_transaction_short_response transaction_short;
  
    uint64_t amount_in = getInputAmount(tx);
    uint64_t amount_out = getOutputAmount(tx);
  
    transaction_short.hash = Common::podToHex(getObjectHash(tx));
    transaction_short.fee = amount_in < amount_out + parameters::MINIMUM_FEE ? parameters::MINIMUM_FEE : amount_in - amount_out;
    transaction_short.amount_out = amount_out;
    transaction_short.size = getObjectBinarySize(tx);
    res.transactions.push_back(transaction_short);  
  }
  res.status = CORE_RPC_STATUS_OK;
  return true;
}

bool RpcServer::f_on_mempool_json(const COMMAND_RPC_GET_MEMPOOL::request& req, COMMAND_RPC_GET_MEMPOOL::response& res) {
  auto pool = m_core.getMemoryPool();
  for (const CryptoNote::tx_memory_pool::TransactionDetails txd : pool) {
    f_mempool_transaction_response mempool_transaction;
    uint64_t amount_out = getOutputAmount(txd.tx);

    mempool_transaction.hash = Common::podToHex(txd.id);
    mempool_transaction.fee = txd.fee;
    mempool_transaction.amount_out = amount_out;
    mempool_transaction.size = txd.blobSize;
	mempool_transaction.receiveTime = txd.receiveTime;
    mempool_transaction.keptByBlock = txd.keptByBlock;
    mempool_transaction.max_used_block_height = txd.maxUsedBlock.height;
    mempool_transaction.max_used_block_id = Common::podToHex(txd.maxUsedBlock.id);
    mempool_transaction.last_failed_height = txd.lastFailedBlock.height;
    mempool_transaction.last_failed_id = Common::podToHex(txd.lastFailedBlock.id);
	res.mempool.push_back(mempool_transaction);
  }
  res.status = CORE_RPC_STATUS_OK;
  return true;
}

bool RpcServer::f_getMixin(const Transaction& transaction, uint64_t& mixin) {
  mixin = 0;
  for (const TransactionInput& txin : transaction.inputs) {
    if (txin.type() != typeid(KeyInput)) {
      continue;
    }
    uint64_t currentMixin = boost::get<KeyInput>(txin).outputIndexes.size();
    if (currentMixin > mixin) {
      mixin = currentMixin;
    }
  }
  return true;
}

bool RpcServer::k_on_transactions_by_payment_id(const K_COMMAND_RPC_GET_TRANSACTIONS_BY_PAYMENT_ID::request& req, K_COMMAND_RPC_GET_TRANSACTIONS_BY_PAYMENT_ID::response& res) {
	if (!req.payment_id.size()) {
		throw JsonRpc::JsonRpcError{ CORE_RPC_ERROR_CODE_WRONG_PARAM, "Wrong parameters, expected payment_id" };
	}
	logger(Logging::INFO, Logging::WHITE) << "RPC request came: Search by Payment ID: " << req.payment_id;

	Crypto::Hash paymentId;
	std::vector<Transaction> transactions;

	if (!parse_hash256(req.payment_id, paymentId)) {
		throw JsonRpc::JsonRpcError{
			CORE_RPC_ERROR_CODE_WRONG_PARAM,
			"Failed to parse Payment ID: " + req.payment_id + '.' };
	}

	if (!m_core.getTransactionsByPaymentId(paymentId, transactions)) {
		throw JsonRpc::JsonRpcError{
			CORE_RPC_ERROR_CODE_INTERNAL_ERROR,
			"Internal error: can't get transactions by Payment ID: " + req.payment_id + '.' };
	}

	for (const Transaction& tx : transactions) {
		f_transaction_short_response transaction_short;
		uint64_t amount_in = 0;
		get_inputs_money_amount(tx, amount_in);
		uint64_t amount_out = get_outs_money_amount(tx);

		transaction_short.hash = Common::podToHex(getObjectHash(tx));
		transaction_short.fee = amount_in - amount_out;
		transaction_short.amount_out = amount_out;
		transaction_short.size = getObjectBinarySize(tx);
		res.transactions.push_back(transaction_short);
	}

	res.status = CORE_RPC_STATUS_OK;
	return true;
}

bool RpcServer::on_getblockcount(const COMMAND_RPC_GETBLOCKCOUNT::request& req, COMMAND_RPC_GETBLOCKCOUNT::response& res) {
  res.count = m_core.get_current_blockchain_height();
  res.status = CORE_RPC_STATUS_OK;
  return true;
}

bool RpcServer::on_getblockhash(const COMMAND_RPC_GETBLOCKHASH::request& req, COMMAND_RPC_GETBLOCKHASH::response& res) {
  if (req.size() != 1) {
    throw JsonRpc::JsonRpcError{ CORE_RPC_ERROR_CODE_WRONG_PARAM, "Wrong parameters, expected height" };
  }

  uint32_t h = static_cast<uint32_t>(req[0]);
  Crypto::Hash blockId = m_core.getBlockIdByHeight(h);
  if (blockId == NULL_HASH) {
    throw JsonRpc::JsonRpcError{ 
      CORE_RPC_ERROR_CODE_TOO_BIG_HEIGHT,
      std::string("To big height: ") + std::to_string(h) + ", current blockchain height = " + std::to_string(m_core.get_current_blockchain_height())
    };
  }

  res = Common::podToHex(blockId);
  return true;
}

namespace {
  uint64_t slow_memmem(void* start_buff, size_t buflen, void* pat, size_t patlen)
  {
    void* buf = start_buff;
    void* end = (char*)buf + buflen - patlen;
    while ((buf = memchr(buf, ((char*)pat)[0], buflen)))
    {
      if (buf>end)
        return 0;
      if (memcmp(buf, pat, patlen) == 0)
        return (char*)buf - (char*)start_buff;
      buf = (char*)buf + 1;
    }
    return 0;
  }
}

bool RpcServer::on_getblocktemplate(const COMMAND_RPC_GETBLOCKTEMPLATE::request& req, COMMAND_RPC_GETBLOCKTEMPLATE::response& res) {
  if (req.reserve_size > TX_EXTRA_NONCE_MAX_COUNT) {
    throw JsonRpc::JsonRpcError{ CORE_RPC_ERROR_CODE_TOO_BIG_RESERVE_SIZE, "To big reserved size, maximum 255" };
  }

  AccountPublicAddress acc = boost::value_initialized<AccountPublicAddress>();

  if (!req.wallet_address.size() || !m_core.currency().parseAccountAddressString(req.wallet_address, acc)) {
    throw JsonRpc::JsonRpcError{ CORE_RPC_ERROR_CODE_WRONG_WALLET_ADDRESS, "Failed to parse wallet address" };
  }

  Block b = boost::value_initialized<Block>();
  CryptoNote::BinaryArray blob_reserve;
  blob_reserve.resize(req.reserve_size, 0);
  if (!m_core.get_block_template(b, acc, res.difficulty, res.height, blob_reserve)) {
    logger(ERROR) << "Failed to create block template";
    throw JsonRpc::JsonRpcError{ CORE_RPC_ERROR_CODE_INTERNAL_ERROR, "Internal error: failed to create block template" };
  }

  BinaryArray block_blob = toBinaryArray(b);
  PublicKey tx_pub_key = CryptoNote::getTransactionPublicKeyFromExtra(b.baseTransaction.extra);
  if (tx_pub_key == NULL_PUBLIC_KEY) {
    logger(ERROR) << "Failed to find tx pub key in coinbase extra";
    throw JsonRpc::JsonRpcError{ CORE_RPC_ERROR_CODE_INTERNAL_ERROR, "Internal error: failed to find tx pub key in coinbase extra" };
  }

  if (0 < req.reserve_size) {
    res.reserved_offset = slow_memmem((void*)block_blob.data(), block_blob.size(), &tx_pub_key, sizeof(tx_pub_key));
    if (!res.reserved_offset) {
      logger(ERROR) << "Failed to find tx pub key in blockblob";
      throw JsonRpc::JsonRpcError{ CORE_RPC_ERROR_CODE_INTERNAL_ERROR, "Internal error: failed to create block template" };
    }
    res.reserved_offset += sizeof(tx_pub_key) + 3; //3 bytes: tag for TX_EXTRA_TAG_PUBKEY(1 byte), tag for TX_EXTRA_NONCE(1 byte), counter in TX_EXTRA_NONCE(1 byte)
    if (res.reserved_offset + req.reserve_size > block_blob.size()) {
      logger(ERROR) << "Failed to calculate offset for reserved bytes";
      throw JsonRpc::JsonRpcError{ CORE_RPC_ERROR_CODE_INTERNAL_ERROR, "Internal error: failed to create block template" };
    }
  } else {
    res.reserved_offset = 0;
  }

  BinaryArray hashing_blob;
  if (!get_block_hashing_blob(b, hashing_blob)) {
	  logger(ERROR) << "Failed to get blockhashing_blob";
	  throw JsonRpc::JsonRpcError{ CORE_RPC_ERROR_CODE_INTERNAL_ERROR, "Internal error: failed to get blockhashing_blob" };
  }

  res.blocktemplate_blob = toHex(block_blob);
  res.blockhashing_blob = toHex(hashing_blob);
  res.status = CORE_RPC_STATUS_OK;

  return true;
}

bool RpcServer::on_get_currency_id(const COMMAND_RPC_GET_CURRENCY_ID::request& /*req*/, COMMAND_RPC_GET_CURRENCY_ID::response& res) {
  Hash currencyId = m_core.currency().genesisBlockHash();
  res.currency_id_blob = Common::podToHex(currencyId);
  return true;
}

bool RpcServer::on_submitblock(const COMMAND_RPC_SUBMITBLOCK::request& req, COMMAND_RPC_SUBMITBLOCK::response& res) {
  if (req.size() != 1) {
    throw JsonRpc::JsonRpcError{ CORE_RPC_ERROR_CODE_WRONG_PARAM, "Wrong param" };
  }

  BinaryArray blockblob;
  if (!fromHex(req[0], blockblob)) {
    throw JsonRpc::JsonRpcError{ CORE_RPC_ERROR_CODE_WRONG_BLOCKBLOB, "Wrong block blob" };
  }

  block_verification_context bvc = boost::value_initialized<block_verification_context>();

  m_core.handle_incoming_block_blob(blockblob, bvc, true, true);

  if (!bvc.m_added_to_main_chain) {
    throw JsonRpc::JsonRpcError{ CORE_RPC_ERROR_CODE_BLOCK_NOT_ACCEPTED, "Block not accepted" };
  }

  res.status = CORE_RPC_STATUS_OK;
  return true;
}


namespace {
  uint64_t get_block_reward(const Block& blk) {
    uint64_t reward = 0;
    for (const TransactionOutput& out : blk.baseTransaction.outputs) {
      reward += out.amount;
    }
    return reward;
  }
}

void RpcServer::fill_block_header_response(const Block& blk, bool orphan_status, uint64_t height, const Hash& hash, block_header_response& responce) {
  responce.major_version = blk.majorVersion;
  responce.minor_version = blk.minorVersion;
  responce.timestamp = blk.timestamp;
  responce.prev_hash = Common::podToHex(blk.previousBlockHash);
  responce.nonce = blk.nonce;
  responce.orphan_status = orphan_status;
  responce.height = height;
  responce.depth = m_core.get_current_blockchain_height() - height - 1;
  responce.hash = Common::podToHex(hash);
  m_core.getBlockDifficulty(static_cast<uint32_t>(height), responce.difficulty);
  responce.reward = get_block_reward(blk);
}

bool RpcServer::on_get_last_block_header(const COMMAND_RPC_GET_LAST_BLOCK_HEADER::request& req, COMMAND_RPC_GET_LAST_BLOCK_HEADER::response& res) {
  uint32_t last_block_height;
  Hash last_block_hash;
  
  m_core.get_blockchain_top(last_block_height, last_block_hash);

  Block last_block;
  if (!m_core.getBlockByHash(last_block_hash, last_block)) {
    throw JsonRpc::JsonRpcError{ CORE_RPC_ERROR_CODE_INTERNAL_ERROR, "Internal error: can't get last block hash." };
  }
  Crypto::Hash tmp_hash = m_core.getBlockIdByHeight(last_block_height);
  bool is_orphaned = last_block_hash != tmp_hash;
  fill_block_header_response(last_block, is_orphaned, last_block_height, last_block_hash, res.block_header);
  res.status = CORE_RPC_STATUS_OK;
  return true;
}

bool RpcServer::on_get_block_header_by_hash(const COMMAND_RPC_GET_BLOCK_HEADER_BY_HASH::request& req, COMMAND_RPC_GET_BLOCK_HEADER_BY_HASH::response& res) {
  Hash block_hash;

  if (!parse_hash256(req.hash, block_hash)) {
    throw JsonRpc::JsonRpcError{
      CORE_RPC_ERROR_CODE_WRONG_PARAM,
      "Failed to parse hex representation of block hash. Hex = " + req.hash + '.' };
  }

  Block blk;
  if (!m_core.getBlockByHash(block_hash, blk)) {
    throw JsonRpc::JsonRpcError{
      CORE_RPC_ERROR_CODE_INTERNAL_ERROR,
      "Internal error: can't get block by hash. Hash = " + req.hash + '.' };
  }

  if (blk.baseTransaction.inputs.front().type() != typeid(BaseInput)) {
    throw JsonRpc::JsonRpcError{
      CORE_RPC_ERROR_CODE_INTERNAL_ERROR,
      "Internal error: coinbase transaction in the block has the wrong type" };
  }

  uint64_t block_height = boost::get<BaseInput>(blk.baseTransaction.inputs.front()).blockIndex;
  Crypto::Hash tmp_hash = m_core.getBlockIdByHeight(block_height);
  bool is_orphaned = block_hash != tmp_hash;
  fill_block_header_response(blk, is_orphaned, block_height, block_hash, res.block_header);
  res.status = CORE_RPC_STATUS_OK;
  return true;
}

bool RpcServer::on_get_block_header_by_height(const COMMAND_RPC_GET_BLOCK_HEADER_BY_HEIGHT::request& req, COMMAND_RPC_GET_BLOCK_HEADER_BY_HEIGHT::response& res) {
  if (m_core.get_current_blockchain_height() <= req.height) {
    throw JsonRpc::JsonRpcError{ CORE_RPC_ERROR_CODE_TOO_BIG_HEIGHT,
      std::string("To big height: ") + std::to_string(req.height) + ", current blockchain height = " + std::to_string(m_core.get_current_blockchain_height()) };
  }

  Hash block_hash = m_core.getBlockIdByHeight(static_cast<uint32_t>(req.height));
  Block blk;
  if (!m_core.getBlockByHash(block_hash, blk)) {
    throw JsonRpc::JsonRpcError{ CORE_RPC_ERROR_CODE_INTERNAL_ERROR,
      "Internal error: can't get block by height. Height = " + std::to_string(req.height) + '.' };
  }
  
  Crypto::Hash tmp_hash = m_core.getBlockIdByHeight(req.height);
  bool is_orphaned = block_hash != tmp_hash;
  fill_block_header_response(blk, is_orphaned, req.height, block_hash, res.block_header);
  res.status = CORE_RPC_STATUS_OK;
  return true;
}

bool RpcServer::k_on_check_tx_key(const K_COMMAND_RPC_CHECK_TX_KEY::request& req, K_COMMAND_RPC_CHECK_TX_KEY::response& res) {
	// parse txid
	Crypto::Hash txid;
	if (!parse_hash256(req.txid, txid)) {
		throw JsonRpc::JsonRpcError{ CORE_RPC_ERROR_CODE_WRONG_PARAM, "Failed to parse txid" };
	}
	// parse address
	CryptoNote::AccountPublicAddress address;
	if (!m_core.currency().parseAccountAddressString(req.address, address)) {
		throw JsonRpc::JsonRpcError{ CORE_RPC_ERROR_CODE_WRONG_PARAM, "Failed to parse address " + req.address + '.' };
	}
	// parse txkey
	Crypto::Hash tx_key_hash;
	size_t size;
	if (!Common::fromHex(req.txkey, &tx_key_hash, sizeof(tx_key_hash), size) || size != sizeof(tx_key_hash)) {
		throw JsonRpc::JsonRpcError{ CORE_RPC_ERROR_CODE_WRONG_PARAM, "Failed to parse txkey" };
	}
	Crypto::SecretKey tx_key = *(struct Crypto::SecretKey *) &tx_key_hash;

	// fetch tx
	Transaction tx;
	std::vector<Crypto::Hash> tx_ids;
	tx_ids.push_back(txid);
	std::list<Crypto::Hash> missed_txs;
	std::list<Transaction> txs;
	m_core.getTransactions(tx_ids, txs, missed_txs, true);

	if (1 == txs.size()) {
		tx = txs.front();
	}
	else {
		throw JsonRpc::JsonRpcError{
			CORE_RPC_ERROR_CODE_WRONG_PARAM,
			"Couldn't find transaction with hash: " + req.txid + '.' };
	}
	CryptoNote::TransactionPrefix transaction = *static_cast<const TransactionPrefix*>(&tx);

	// obtain key derivation
	Crypto::KeyDerivation derivation;
	if (!Crypto::generate_key_derivation(address.viewPublicKey, tx_key, derivation))
	{
		throw JsonRpc::JsonRpcError{ CORE_RPC_ERROR_CODE_WRONG_PARAM, "Failed to generate key derivation from supplied parameters" };
	}
	
	// look for outputs
	uint64_t received(0);
	size_t keyIndex(0);
	std::vector<TransactionOutput> outputs;
	try {
		for (const TransactionOutput& o : transaction.outputs) {
			if (o.target.type() == typeid(KeyOutput)) {
				const KeyOutput out_key = boost::get<KeyOutput>(o.target);
				Crypto::PublicKey pubkey;
				derive_public_key(derivation, keyIndex, address.spendPublicKey, pubkey);
				if (pubkey == out_key.key) {
					received += o.amount;
					outputs.push_back(o);
				}
			}
			++keyIndex;
		}
	}
	catch (...)
	{
		throw JsonRpc::JsonRpcError{ CORE_RPC_ERROR_CODE_INTERNAL_ERROR, "Unknown error" };
	}
	res.amount = received;
	res.outputs = outputs;
	res.status = CORE_RPC_STATUS_OK;
	return true;
}

bool RpcServer::k_on_check_tx_with_view_key(const K_COMMAND_RPC_CHECK_TX_WITH_PRIVATE_VIEW_KEY::request& req, K_COMMAND_RPC_CHECK_TX_WITH_PRIVATE_VIEW_KEY::response& res) {
	// parse txid
	Crypto::Hash txid;
	if (!parse_hash256(req.txid, txid)) {
		throw JsonRpc::JsonRpcError{ CORE_RPC_ERROR_CODE_WRONG_PARAM, "Failed to parse txid" };
	}
	// parse address
	CryptoNote::AccountPublicAddress address;
	if (!m_core.currency().parseAccountAddressString(req.address, address)) {
		throw JsonRpc::JsonRpcError{ CORE_RPC_ERROR_CODE_WRONG_PARAM, "Failed to parse address " + req.address + '.' };
	}
	// parse view key
	Crypto::Hash view_key_hash;
	size_t size;
	if (!Common::fromHex(req.view_key, &view_key_hash, sizeof(view_key_hash), size) || size != sizeof(view_key_hash)) {
		throw JsonRpc::JsonRpcError{ CORE_RPC_ERROR_CODE_WRONG_PARAM, "Failed to parse private view key" };
	}
	Crypto::SecretKey viewKey = *(struct Crypto::SecretKey *) &view_key_hash;

	// fetch tx
	Transaction tx;
	std::vector<Crypto::Hash> tx_ids;
	tx_ids.push_back(txid);
	std::list<Crypto::Hash> missed_txs;
	std::list<Transaction> txs;
	m_core.getTransactions(tx_ids, txs, missed_txs, true);

	if (1 == txs.size()) {
		tx = txs.front();
	}
	else {
		throw JsonRpc::JsonRpcError{
			CORE_RPC_ERROR_CODE_WRONG_PARAM,
			"Couldn't find transaction with hash: " + req.txid + '.' };
	}
	CryptoNote::TransactionPrefix transaction = *static_cast<const TransactionPrefix*>(&tx);
	
	// get tx pub key
	Crypto::PublicKey txPubKey = getTransactionPublicKeyFromExtra(transaction.extra);

	// obtain key derivation
	Crypto::KeyDerivation derivation;
	if (!Crypto::generate_key_derivation(txPubKey, viewKey, derivation))
	{
		throw JsonRpc::JsonRpcError{ CORE_RPC_ERROR_CODE_WRONG_PARAM, "Failed to generate key derivation from supplied parameters" };
	}

	// look for outputs
	uint64_t received(0);
	size_t keyIndex(0);
	std::vector<TransactionOutput> outputs;
	try {
		for (const TransactionOutput& o : transaction.outputs) {
			if (o.target.type() == typeid(KeyOutput)) {
				const KeyOutput out_key = boost::get<KeyOutput>(o.target);
				Crypto::PublicKey pubkey;
				derive_public_key(derivation, keyIndex, address.spendPublicKey, pubkey);
				if (pubkey == out_key.key) {
					received += o.amount;
					outputs.push_back(o);
				}
			}
			++keyIndex;
		}
	}
	catch (...)
	{
		throw JsonRpc::JsonRpcError{ CORE_RPC_ERROR_CODE_INTERNAL_ERROR, "Unknown error" };
	}
	res.amount = received;
	res.outputs = outputs;
	
	Crypto::Hash blockHash;
	uint32_t blockHeight;
	if (m_core.getBlockContainingTx(txid, blockHash, blockHeight)) {
		res.confirmations = m_protocolQuery.getObservedHeight() - blockHeight;
	}

	res.status = CORE_RPC_STATUS_OK;
	return true;
}

bool RpcServer::on_validate_address(const COMMAND_RPC_VALIDATE_ADDRESS::request& req, COMMAND_RPC_VALIDATE_ADDRESS::response& res) {
  AccountPublicAddress acc = boost::value_initialized<AccountPublicAddress>();
  bool r = m_core.currency().parseAccountAddressString(req.address, acc);
  res.isvalid = r;
  if (r) {
    res.address = m_core.currency().accountAddressAsString(acc);
    res.spendPublicKey = Common::podToHex(acc.spendPublicKey);
    res.viewPublicKey = Common::podToHex(acc.viewPublicKey);
  }
  res.status = CORE_RPC_STATUS_OK;
  return true;
}

bool RpcServer::on_verify_message(const COMMAND_RPC_VERIFY_MESSAGE::request& req, COMMAND_RPC_VERIFY_MESSAGE::response& res) {
	Crypto::Hash hash;
	Crypto::cn_fast_hash(req.message.data(), req.message.size(), hash);

	AccountPublicAddress acc = boost::value_initialized<AccountPublicAddress>();
	if (!m_core.currency().parseAccountAddressString(req.address, acc)) {
		throw JsonRpc::JsonRpcError(CORE_RPC_ERROR_CODE_WRONG_PARAM, std::string("Failed to parse address"));
	}

	const size_t header_len = strlen("SigV1");
	if (req.signature.size() < header_len || req.signature.substr(0, header_len) != "SigV1") {
		throw JsonRpc::JsonRpcError(CORE_RPC_ERROR_CODE_WRONG_PARAM, std::string("Signature header check error"));
	}
	std::string decoded;
	Crypto::Signature s;
	if (!Tools::Base58::decode(req.signature.substr(header_len), decoded) || sizeof(s) != decoded.size()) {
		throw JsonRpc::JsonRpcError(CORE_RPC_ERROR_CODE_WRONG_PARAM, std::string("Signature decoding error"));
		return false;
	}
	memcpy(&s, decoded.data(), sizeof(s));
	res.sig_valid = Crypto::check_signature(hash, acc.spendPublicKey, s);
	res.status = CORE_RPC_STATUS_OK;
	return true;
}

}
