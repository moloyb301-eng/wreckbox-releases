"""Records the bytes aioslsk (the library the sidecar uses) puts on the wire for the messages the native client sends and
receives, so tests/slsk_tests.cpp can check the C++ encoders and decoders byte for byte.
   <bundled python> tests/fixtures/make_slsk_wire_golden.py > tests/fixtures/slsk_wire.json
(the bundled interpreter has aioslsk: build\\Release\\soulseek\\python\\python.exe)."""
import json
import sys
import zlib

from aioslsk.protocol import messages as m
from aioslsk.protocol.primitives import Attribute, FileData, calc_md5

out = {}


def put(name, data):
    out[name] = data.hex()


user, pw = "dj_test", "p@ssw0rd"
put("login_request", m.Login.Request(username=user, password=pw, client_version=175, md5hash=calc_md5(user + pw), minor_version=1).serialize())
put("login_ok", m.Login.Response(success=True, greeting="Welcome!", ip="203.0.113.7", md5hash=calc_md5(pw), privileged=False).serialize())
put("login_fail", m.Login.Response(success=False, reason="INVALIDPASS").serialize())
put("set_listen_port", m.SetListenPort.Request(port=60000).serialize())
put("set_status", m.SetStatus.Request(status=2).serialize())
put("shared_folders_files", m.SharedFoldersFiles.Request(shared_folder_count=3, shared_file_count=1234).serialize())
put("accept_children", m.AcceptChildren.Request(accept=False).serialize())
put("toggle_parent_search", m.ToggleParentSearch.Request(enable=False).serialize())
put("ping", m.Ping.Request().serialize())
put("file_search", m.FileSearch.Request(ticket=4242, query="skrillex selecta").serialize())
put("get_peer_address", m.GetPeerAddress.Request(username="bob").serialize())
put("peer_address", m.GetPeerAddress.Response(username="bob", ip="198.51.100.20", port=2234, obfuscated_port_amount=0, obfuscated_port=0).serialize())
put("connect_to_peer_request", m.ConnectToPeer.Request(ticket=77, username="bob", typ="P").serialize())
put("connect_to_peer_response", m.ConnectToPeer.Response(username="bob", typ="F", ip="198.51.100.20", port=2234, ticket=99, privileged=False).serialize())
put("peer_init", m.PeerInit.Request(username=user, typ="P", ticket=0).serialize())
put("pierce_firewall", m.PeerPierceFirewall.Request(ticket=1234567).serialize())
put("queue_upload", m.PeerTransferQueue.Request(filename="Music\\Skrillex\\Selecta ✓.flac").serialize())
put("transfer_request_upload", m.PeerTransferRequest.Request(direction=1, ticket=555, filename="Music\\Skrillex\\Selecta.flac", filesize=30_000_000).serialize())
put("transfer_request_download", m.PeerTransferRequest.Request(direction=0, ticket=556, filename="a\\b.mp3").serialize())
put("transfer_reply_ok", m.PeerTransferReply.Request(ticket=555, allowed=True).serialize())
put("transfer_reply_size", m.PeerTransferReply.Request(ticket=555, allowed=True, filesize=30_000_000).serialize())
put("transfer_reply_no", m.PeerTransferReply.Request(ticket=555, allowed=False, reason="Cancelled").serialize())
put("place_in_queue_request", m.PeerPlaceInQueueRequest.Request(filename="a\\b.mp3").serialize())
put("place_in_queue", m.PeerPlaceInQueueReply.Request(filename="a\\b.mp3", place=7).serialize())
put("upload_failed", m.PeerUploadFailed.Request(filename="a\\b.mp3").serialize())
put("queue_failed", m.PeerTransferQueueFailed.Request(filename="a\\b.mp3", reason="File not shared.").serialize())

files = [
    FileData(unknown=1, filename="Music\\Skrillex\\Selecta.flac", filesize=30_000_000, extension="flac",
             attributes=[Attribute(key=1, value=190), Attribute(key=4, value=44100), Attribute(key=5, value=16)]),
    FileData(unknown=1, filename="Music\\Skrillex\\Selecta.mp3", filesize=8_000_000, extension="mp3", attributes=[Attribute(key=0, value=320), Attribute(key=1, value=190)]),
    FileData(unknown=1, filename="Ünïcode\\Zoë ✓.ogg", filesize=1, extension="", attributes=[]),
]
reply = m.PeerSearchReply.Request(username="bob", ticket=4242, results=files, has_slots_free=True, avg_speed=2_000_000, queue_size=3, unknown=0, locked_results=None)
raw = reply.serialize(compress=False)
put("search_reply_uncompressed_body", raw[8:])   # the payload before zlib: what the C++ must decode after inflating
put("search_reply", reply.serialize())            # compressed, as on the wire
out["search_reply_inflated_matches"] = zlib.decompress(bytes.fromhex(out["search_reply"])[8:]).hex() == out["search_reply_uncompressed_body"]
json.dump(out, sys.stdout, indent=1, sort_keys=True)
