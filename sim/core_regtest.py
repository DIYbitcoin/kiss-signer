#!/usr/bin/env python3
"""Run one KISS native-SegWit PSBT round trip on an isolated Core regtest.

Bitcoin Core is only the transaction generator, finalizer and consensus oracle
for this test. It is not a recommended wallet or coordinator.
"""
from __future__ import annotations

import base64
import decimal
import json
import os
import pathlib
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parents[1]
RECIPIENT_SATS = 500_000
INPUT_SATS = 2_000_000
SAT = decimal.Decimal(100_000_000)
BECH32 = "qpzry9x8gf2tvdw0s3jn54khce6mua7l"


def btc_to_sats(value: object) -> int:
    return int(decimal.Decimal(str(value)) * SAT)


class CoreRPC:
    def __init__(self, port: int, cookie: pathlib.Path):
        self.url = f"http://127.0.0.1:{port}/"
        self.cookie = cookie
        self.seq = 0

    def call(self, method: str, *params: object, wallet: str | None = None):
        self.seq += 1
        url = self.url
        if wallet:
            url += "wallet/" + urllib.parse.quote(wallet, safe="")
        body = json.dumps({"jsonrpc": "1.0", "id": self.seq,
                           "method": method, "params": params}).encode()
        auth = self.cookie.read_text().strip().encode()
        request = urllib.request.Request(
            url, body, {"Authorization": "Basic " + base64.b64encode(auth).decode(),
                        "Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(request, timeout=10) as response:
                result = json.loads(response.read())
        except urllib.error.HTTPError as exc:
            detail = exc.read().decode(errors="replace")
            raise RuntimeError(f"Core RPC {method} returned HTTP {exc.code}: {detail}") from exc
        if result.get("error"):
            raise RuntimeError(f"Core RPC {method}: {result['error']}")
        return result["result"]


def check(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def kiss_address_script(address: str) -> str:
    """Decode a KISS-rendered v0 P2WPKH address into its scriptPubKey hex."""
    lowered = address.lower()
    check(address == lowered or address.upper() == address,
          f"mixed-case SegWit address from KISS: {address}")
    hrp, sep, encoded = lowered.rpartition("1")
    check(bool(sep) and hrp in ("tb", "bcrt"),
          f"unexpected KISS test-network address: {address}")
    values = [BECH32.find(char) for char in encoded]
    check(all(value >= 0 for value in values) and len(values) > 6,
          f"invalid Bech32 address from KISS: {address}")
    data = values[:-6]
    check(data[0] == 0, f"expected native SegWit v0 address from KISS: {address}")
    accumulator = bits = 0
    program = bytearray()
    for value in data[1:]:
        accumulator = (accumulator << 5) | value
        bits += 5
        while bits >= 8:
            bits -= 8
            program.append((accumulator >> bits) & 0xFF)
    check(len(program) == 20 and bits < 5 and (accumulator << (8 - bits)) & 0xFF == 0,
          f"invalid P2WPKH program from KISS: {address}")
    return "0014" + program.hex()


def main() -> int:
    bitcoind = os.environ.get("BITCOIND") or shutil.which("bitcoind")
    if not bitcoind:
        raise RuntimeError("bitcoind is required (set BITCOIND to its path)")
    version = subprocess.run([bitcoind, "--version"], check=True, text=True,
                             stdout=subprocess.PIPE).stdout.splitlines()[0]
    print(f"Bitcoin Core: {version}", flush=True)

    with tempfile.TemporaryDirectory(prefix="kiss-core-regtest-") as work:
        workdir = pathlib.Path(work)
        datadir = workdir / "core"
        datadir.mkdir()
        sim_tmp = workdir / "sim"
        sim_tmp.mkdir()
        binary = workdir / "kiss-core-interop"
        subprocess.run(["bash", str(ROOT / "sim/build_sp_interop.sh"), str(binary)],
                       cwd=ROOT, check=True)
        env = os.environ.copy()
        env["KISS_SIM_TMP"] = str(sim_tmp)
        descriptor = subprocess.run([str(binary), "--descriptor"], env=env,
                                    check=True, text=True, stdout=subprocess.PIPE).stdout.strip()
        check(descriptor.startswith("wpkh(") and "<0;1>" in descriptor,
              "KISS did not export the expected native SegWit multipath descriptor")

        # Choose a free local RPC port and disable every peer/network discovery
        # path. Regtest remains entirely local and uses a disposable datadir.
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        daemon_log = workdir / "bitcoind.log"
        log_file = daemon_log.open("w")
        proc = subprocess.Popen([
            bitcoind, f"-datadir={datadir}", "-regtest=1", "-server=1",
            "-daemon=0", f"-rpcport={port}", "-rpcbind=127.0.0.1",
            "-rpcallowip=127.0.0.1", "-listen=0", "-discover=0",
            "-dnsseed=0", "-connect=0", "-fallbackfee=0.0002",
            "-printtoconsole=1"], stdout=log_file, stderr=subprocess.STDOUT)
        cookie = datadir / "regtest" / ".cookie"
        rpc = CoreRPC(port, cookie)
        try:
            deadline = time.monotonic() + 45
            while time.monotonic() < deadline:
                if proc.poll() is not None:
                    raise RuntimeError(f"bitcoind exited with status {proc.returncode}")
                if cookie.exists():
                    try:
                        rpc.call("getblockchaininfo")
                        break
                    except (OSError, RuntimeError):
                        pass
                time.sleep(0.25)
            else:
                raise RuntimeError("timed out waiting for Bitcoin Core RPC")

            check(rpc.call("getblockchaininfo")["chain"] == "regtest",
                  "Core did not start on regtest")
            rpc.call("createwallet", "miner", False, False, "", False, True)

            # Split KISS's public multipath descriptor into Core's receive and
            # change descriptors. No private key leaves the KISS test bridge.
            desc0 = descriptor.replace("<0;1>", "0")
            desc1 = descriptor.replace("<0;1>", "1")
            descriptions = []
            for desc, internal in ((desc0, False), (desc1, True)):
                info = rpc.call("getdescriptorinfo", desc)
                descriptions.append({"desc": info["descriptor"], "timestamp": "now",
                                     "active": True, "internal": internal,
                                     "range": [0, 20]})
            rpc.call("createwallet", "kiss-watch", True, False, "", False, True)
            imported = rpc.call("importdescriptors", descriptions, wallet="kiss-watch")
            check(all(item.get("success") for item in imported),
                  f"Core descriptor import failed: {imported}")

            receive = rpc.call("deriveaddresses", descriptions[0]["desc"], [0, 0])[0]
            change = rpc.call("deriveaddresses", descriptions[1]["desc"], [0, 0])[0]
            mine_to = rpc.call("getnewaddress", "", "bech32", wallet="miner")
            rpc.call("generatetoaddress", 101, mine_to)
            funding_txid = rpc.call("sendtoaddress", receive, INPUT_SATS / 100_000_000,
                                    wallet="miner")
            rpc.call("generatetoaddress", 1, mine_to)

            recipient = rpc.call("getnewaddress", "regtest-recipient", "bech32",
                                 wallet="miner")
            funded = rpc.call("walletcreatefundedpsbt", [],
                              {recipient: RECIPIENT_SATS / 100_000_000}, 0,
                              {"includeWatching": True, "changeAddress": change,
                               "fee_rate": 2, "add_inputs": True}, True,
                              wallet="kiss-watch")
            psbt = base64.b64decode(funded["psbt"], validate=True)
            request_path = workdir / "request.psbt"
            signed_path = workdir / "signed.psbt"
            review_path = workdir / "review.json"
            request_path.write_bytes(psbt)
            subprocess.run([str(binary), str(request_path), str(signed_path),
                            str(review_path)], cwd=ROOT, env=env, check=True)
            review = json.loads(review_path.read_text())

            decoded = rpc.call("decodepsbt", funded["psbt"])
            check(len(decoded["tx"]["vin"]) == 1, "expected one native SegWit input")
            check(decoded["tx"]["vin"][0]["txid"] == funding_txid,
                  "Core PSBT did not spend the regtest funding transaction")
            witness_utxo = decoded["inputs"][0].get("witness_utxo", {})
            check(witness_utxo.get("scriptPubKey", "").startswith("0014"),
                  "Core PSBT input is not native SegWit P2WPKH")
            check(decoded["inputs"][0].get("non_witness_utxo"),
                  "Core PSBT omitted the full previous transaction needed to prove the input amount")
            funding_info = rpc.call("gettransaction", funding_txid, wallet="miner")
            prev = rpc.call("getrawtransaction", funding_txid, True,
                            funding_info["blockhash"])
            prevout = decoded["tx"]["vin"][0]["vout"]
            expected_input = btc_to_sats(prev["vout"][prevout]["value"])
            expected_fee = btc_to_sats(funded["fee"])
            check(review["status"] == 0, "KISS review did not return READY")
            check(review["input_sats"] == expected_input,
                  f"KISS input review {review['input_sats']} != Core {expected_input} sats")
            check(review["send_sats"] == RECIPIENT_SATS,
                  f"KISS recipient amount is {review['send_sats']} sats")
            check(review["fee_sats"] == expected_fee,
                  f"KISS fee review {review['fee_sats']} != Core {expected_fee} sats")
            outputs = review["outputs"]
            decoded_outputs = decoded["tx"]["vout"]
            by_script = {entry["scriptPubKey"]["hex"]: btc_to_sats(entry["value"])
                         for entry in decoded_outputs}
            recipient_script = rpc.call("getaddressinfo", recipient,
                                        wallet="miner")["scriptPubKey"]
            change_script = rpc.call("getaddressinfo", change,
                                     wallet="miner")["scriptPubKey"]
            pay = [out for out in outputs
                   if kiss_address_script(out["address"]) == recipient_script and
                   by_script.get(recipient_script) == out["sats"] and
                   out["sats"] == RECIPIENT_SATS]
            change_out = [out for out in outputs if out["change"]]
            check(len(pay) == 1 and
                  by_script.get(recipient_script) == RECIPIENT_SATS,
                  "KISS review did not identify Core's recipient and exact amount")
            check(len(change_out) == 1 and
                  kiss_address_script(change_out[0]["address"]) == change_script and
                  by_script.get(change_script) ==
                  change_out[0]["sats"],
                  "KISS review did not classify the expected wallet change")
            check(change_out[0]["sats"] == expected_input - RECIPIENT_SATS - expected_fee,
                  "KISS change amount does not match Core's transaction accounting")

            signed_b64 = base64.b64encode(signed_path.read_bytes()).decode()
            finalized = rpc.call("finalizepsbt", signed_b64, True)
            check(finalized.get("complete") is True and finalized.get("hex"),
                  "Core could not finalize the PSBT using KISS's signatures")
            accepted = rpc.call("testmempoolaccept", [finalized["hex"]])[0]
            check(accepted.get("allowed") is True,
                  f"Core rejected the signed transaction: {accepted}")
            txid = rpc.call("sendrawtransaction", finalized["hex"])
            confirmed_block = rpc.call("generatetoaddress", 1, mine_to)[0]
            confirmed = rpc.call("getrawtransaction", txid, True, confirmed_block)
            check(confirmed.get("confirmations", 0) >= 1,
                  "Core broadcast the transaction but did not confirm it")
            print("PASS: KISS review -> sign/export -> Core finalize -> mempool -> confirmed regtest")
            print(f"      input={expected_input} sats recipient={RECIPIENT_SATS} sats "
                  f"change={change_out[0]['sats']} sats fee={expected_fee} sats")
            return 0
        except Exception:
            log_file.flush()
            print("--- bitcoind diagnostic log ---", file=sys.stderr)
            print(daemon_log.read_text(errors="replace")[-12000:], file=sys.stderr)
            raise
        finally:
            if proc.poll() is None:
                try:
                    rpc.call("stop")
                    proc.wait(timeout=15)
                except Exception:
                    proc.terminate()
                    try:
                        proc.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                        proc.wait(timeout=5)
            log_file.close()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)
