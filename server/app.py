"""
Cloud print server — receives documents via web UI, renders to PBM,
delivers to ESP32 printer bridge via HTTP polling.
"""

import os
import uuid
import json
import shutil
import time
from pathlib import Path
from datetime import datetime

from flask import Flask, render_template, request, jsonify, send_file, abort
from flask_socketio import SocketIO, emit
from flask_login import LoginManager, login_user, login_required, UserMixin, current_user
from dotenv import load_dotenv

from renderer import (render_pdf_to_pbm, render_image_to_pbm, render_text_to_pbm,
                      convert_pbm_to_zjs)

load_dotenv()

app = Flask(__name__)
app.config["SECRET_KEY"] = os.getenv("SECRET_KEY", "dev-secret-change-me")
app.config["MAX_CONTENT_LENGTH"] = 50 * 1024 * 1024  # 50 MB upload limit

JOBS_DIR = Path(os.getenv("JOBS_DIR", "./jobs"))
JOBS_DIR.mkdir(exist_ok=True)

DEVICE_API_KEY = os.getenv("DEVICE_API_KEY", "change-me")
WEB_PASSWORD = os.getenv("WEB_PASSWORD", "admin")

socketio = SocketIO(app, cors_allowed_origins="*", async_mode="threading")

login_manager = LoginManager(app)
login_manager.login_view = "login_page"

jobs_db: dict[str, dict] = {}
printer_status: dict = {"state": 0, "wifi": False}
last_device_seen: float = 0.0


# ──── Auth ────

class User(UserMixin):
    id = "admin"

THE_USER = User()

@login_manager.user_loader
def load_user(uid):
    return THE_USER if uid == "admin" else None


def _check_device_key():
    auth = request.headers.get("Authorization", "")
    if not auth.endswith(DEVICE_API_KEY):
        abort(403)


# ──── Web routes ────

@app.route("/")
@login_required
def index():
    return render_template("index.html")


@app.route("/login", methods=["GET", "POST"])
def login_page():
    if request.method == "POST":
        pw = request.form.get("password", "")
        if pw == WEB_PASSWORD:
            login_user(THE_USER, remember=True)
            return jsonify({"ok": True})
        return jsonify({"ok": False, "error": "Wrong password"}), 401
    return render_template("index.html", login=True)


# ──── Print API ────

@app.route("/api/print", methods=["POST"])
@login_required
def api_print():
    f = request.files.get("document")
    if not f or not f.filename:
        return jsonify({"error": "No file"}), 400

    try:
        settings = {
            "copies": int(request.form.get("copies", 1)),
            "page_range": request.form.get("page_range", "all"),
            "orientation": request.form.get("orientation", "portrait"),
            "paper": request.form.get("paper", "letter"),
            "dpi": int(request.form.get("dpi", 600)),
            "media_type": int(request.form.get("media_type", 1)),
        }
    except (ValueError, TypeError) as e:
        return jsonify({"error": f"Invalid setting: {e}"}), 400

    file_bytes = f.read()
    ext = Path(f.filename).suffix.lower()

    try:
        if ext == ".pdf":
            pages = render_pdf_to_pbm(file_bytes, **{k: settings[k] for k in
                                      ("dpi", "page_range", "orientation", "paper")})
        elif ext in (".png", ".jpg", ".jpeg", ".tiff", ".tif", ".bmp"):
            pages = render_image_to_pbm(file_bytes, dpi=settings["dpi"],
                                         orientation=settings["orientation"],
                                         paper=settings["paper"])
        elif ext in (".txt", ".text", ".log"):
            pages = render_text_to_pbm(file_bytes.decode("utf-8", errors="replace"),
                                        dpi=settings["dpi"], paper=settings["paper"])
        else:
            return jsonify({"error": f"Unsupported format: {ext}"}), 400
    except Exception as e:
        return jsonify({"error": str(e)}), 500

    total_pages = len(pages) * settings["copies"]
    job_id = uuid.uuid4().hex[:12]
    job_dir = JOBS_DIR / job_id
    job_dir.mkdir()

    try:
        for i, pbm in enumerate(pages):
            (job_dir / f"page_{i}.pbm").write_bytes(pbm)
            zjs = convert_pbm_to_zjs(pbm, dpi=settings["dpi"],
                                     paper=settings["paper"],
                                     media_type=settings["media_type"])
            (job_dir / f"page_{i}.zjs").write_bytes(zjs)
    except Exception as e:
        shutil.rmtree(job_dir, ignore_errors=True)
        return jsonify({"error": f"Conversion failed: {e}"}), 500

    job = {
        "id": job_id,
        "filename": f.filename,
        "total_pages": total_pages,
        "rendered_pages": len(pages),
        "copies": settings["copies"],
        "status": "queued",
        "created": datetime.utcnow().isoformat(),
        "progress": 0,
    }
    jobs_db[job_id] = job

    app.logger.info("Job %s queued (%d pages), ESP32 will pick up on next poll", job_id, total_pages)

    return jsonify({"job_id": job_id, "pages": total_pages})


@app.route("/api/jobs/<job_id>/page/<int:page_num>.pbm")
def api_get_page(job_id, page_num):
    _check_device_key()
    path = JOBS_DIR / job_id / f"page_{page_num}.pbm"
    if not path.exists():
        abort(404)
    return send_file(path, mimetype="image/x-portable-bitmap")


@app.route("/api/jobs/<job_id>/page/<int:page_num>.zjs")
def api_get_page_zjs(job_id, page_num):
    _check_device_key()
    path = JOBS_DIR / job_id / f"page_{page_num}.zjs"
    if not path.exists():
        abort(404)
    return send_file(path, mimetype="application/octet-stream")


@app.route("/api/status")
@login_required
def api_status():
    online = (time.time() - last_device_seen) < 30
    effective_status = printer_status if online else {"state": 0, "wifi": False}
    return jsonify({
        "printer": effective_status,
        "jobs": list(jobs_db.values())[-20:],
    })


@app.route("/api/jobs/<job_id>/complete", methods=["POST"])
def api_complete_job(job_id):
    _check_device_key()

    if job_id in jobs_db:
        jobs_db[job_id]["status"] = "completed"
        socketio.emit("job_update", jobs_db[job_id], namespace="/ui")

    job_dir = JOBS_DIR / job_id
    if job_dir.exists():
        shutil.rmtree(job_dir)

    return jsonify({"ok": True})


# ──── Device polling API (replaces WebSocket) ────

@app.route("/api/poll")
def api_poll():
    """ESP32 calls this every ~10s to pick up queued jobs."""
    _check_device_key()

    global last_device_seen
    last_device_seen = time.time()

    queued = []
    for jid, j in list(jobs_db.items()):
        if j.get("status") == "queued":
            queued.append({"job_id": jid, "pages": j.get("rendered_pages", 0)})

    return jsonify({"jobs": queued})


@app.route("/api/device/status", methods=["POST"])
def api_device_status():
    """ESP32 posts printer state and/or job progress."""
    _check_device_key()

    global printer_status, last_device_seen
    last_device_seen = time.time()

    data = request.get_json(silent=True)
    if not data:
        return jsonify({"error": "No JSON body"}), 400

    msg_type = data.get("type")

    if msg_type == "printer_status":
        printer_status = {"state": data.get("state", 0), "wifi": data.get("wifi", False)}
        socketio.emit("printer_status", printer_status, namespace="/ui")

    elif msg_type == "job_status":
        job_id = data.get("job_id")
        if job_id and job_id in jobs_db:
            jobs_db[job_id]["status"] = data.get("status", "unknown")
            jobs_db[job_id]["progress"] = data.get("page", 0)
            socketio.emit("job_update", jobs_db[job_id], namespace="/ui")

    return jsonify({"ok": True})


# ──── Socket.IO: Browser UI ────

@socketio.on("connect", namespace="/ui")
def ui_connect():
    online = (time.time() - last_device_seen) < 30
    effective_status = printer_status if online else {"state": 0, "wifi": False}
    emit("printer_status", effective_status)
    emit("jobs_list", list(jobs_db.values())[-20:])


# ──── Entry point ────

if __name__ == "__main__":
    socketio.run(app, host="0.0.0.0", port=5000, debug=True)
