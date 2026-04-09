document.addEventListener("DOMContentLoaded", () => {
  const loginScreen = document.getElementById("login-screen");
  const mainScreen = document.getElementById("main-screen");
  const loginForm = document.getElementById("login-form");
  const loginError = document.getElementById("login-error");

  const dropZone = document.getElementById("drop-zone");
  const fileInput = document.getElementById("file-input");
  const fileInfo = document.getElementById("file-info");
  const fileName = document.getElementById("file-name");
  const fileClear = document.getElementById("file-clear");
  const settings = document.getElementById("settings");
  const printBtn = document.getElementById("print-btn");

  const printerBadge = document.getElementById("printer-badge");
  const jobsList = document.getElementById("jobs-list");

  let selectedFile = null;
  let socket = null;

  // ── Detect login state by trying /api/status ──
  fetch("/api/status").then(r => {
    if (r.ok) { showMain(); } else { showLogin(); }
  }).catch(() => showLogin());

  function showLogin() {
    loginScreen.classList.remove("hidden");
    mainScreen.classList.add("hidden");
  }

  function showMain() {
    loginScreen.classList.add("hidden");
    mainScreen.classList.remove("hidden");
    connectSocket();
    refreshStatus();
  }

  // ── Login ──
  loginForm.addEventListener("submit", async (e) => {
    e.preventDefault();
    const pw = document.getElementById("login-pw").value;
    const res = await fetch("/login", {
      method: "POST",
      headers: { "Content-Type": "application/x-www-form-urlencoded" },
      body: `password=${encodeURIComponent(pw)}`,
    });
    if (res.ok) {
      loginError.classList.add("hidden");
      showMain();
    } else {
      loginError.classList.remove("hidden");
    }
  });

  // ── File selection ──
  dropZone.addEventListener("click", () => fileInput.click());

  dropZone.addEventListener("dragover", (e) => {
    e.preventDefault();
    dropZone.classList.add("dragover");
  });
  dropZone.addEventListener("dragleave", () => dropZone.classList.remove("dragover"));

  dropZone.addEventListener("drop", (e) => {
    e.preventDefault();
    dropZone.classList.remove("dragover");
    if (e.dataTransfer.files.length) selectFile(e.dataTransfer.files[0]);
  });

  fileInput.addEventListener("change", () => {
    if (fileInput.files.length) selectFile(fileInput.files[0]);
  });

  fileClear.addEventListener("click", clearFile);

  function selectFile(file) {
    selectedFile = file;
    fileName.textContent = file.name;
    fileInfo.classList.remove("hidden");
    dropZone.classList.add("hidden");
    settings.classList.remove("hidden");
  }

  function clearFile() {
    selectedFile = null;
    fileInput.value = "";
    fileInfo.classList.add("hidden");
    dropZone.classList.remove("hidden");
    settings.classList.add("hidden");
  }

  // ── Print ──
  printBtn.addEventListener("click", async () => {
    if (!selectedFile) return;
    printBtn.disabled = true;
    printBtn.textContent = "Sending...";

    const form = new FormData();
    form.append("document", selectedFile);
    form.append("copies", document.getElementById("copies").value);
    form.append("page_range", document.getElementById("page-range").value);
    form.append("paper", document.getElementById("paper").value);
    form.append("orientation", document.getElementById("orientation").value);
    form.append("dpi", document.getElementById("dpi").value);

    try {
      const res = await fetch("/api/print", { method: "POST", body: form });
      const data = await res.json();
      if (res.ok) {
        clearFile();
      } else {
        alert(data.error || "Print failed");
      }
    } catch (err) {
      alert("Network error: " + err.message);
    }

    printBtn.disabled = false;
    printBtn.textContent = "Print";
  });

  // ── Socket.IO for live updates ──
  function connectSocket() {
    socket = io("/ui", { transports: ["websocket", "polling"] });

    socket.on("printer_status", (data) => {
      const online = data.state >= 3;
      printerBadge.textContent = online ? "Printer Online" : "Printer Offline";
      printerBadge.className = "badge " + (online ? "online" : "offline");
    });

    socket.on("jobs_list", (jobs) => {
      renderJobs(jobs);
    });

    socket.on("job_update", (job) => {
      updateJob(job);
    });
  }

  function refreshStatus() {
    fetch("/api/status").then(r => r.json()).then(data => {
      if (data.printer) {
        const online = data.printer.state >= 3;
        printerBadge.textContent = online ? "Printer Online" : "Printer Offline";
        printerBadge.className = "badge " + (online ? "online" : "offline");
      }
      if (data.jobs) renderJobs(data.jobs);
    }).catch(() => {});
  }

  // ── Job rendering ──
  const jobsMap = {};

  function renderJobs(jobs) {
    if (!jobs.length) {
      jobsList.innerHTML = '<p class="empty-state">No print jobs yet</p>';
      return;
    }
    jobsList.innerHTML = "";
    jobs.forEach(j => {
      jobsMap[j.id] = j;
      jobsList.appendChild(makeJobEl(j));
    });
  }

  function updateJob(job) {
    jobsMap[job.id] = job;
    const existing = document.getElementById("job-" + job.id);
    if (existing) {
      existing.replaceWith(makeJobEl(job));
    } else {
      const empty = jobsList.querySelector(".empty-state");
      if (empty) empty.remove();
      jobsList.prepend(makeJobEl(job));
    }
  }

  function makeJobEl(job) {
    const el = document.createElement("div");
    el.className = "job-item";
    el.id = "job-" + job.id;

    const pct = job.total_pages > 0 ? Math.round((job.progress / job.total_pages) * 100) : 0;

    el.innerHTML = `
      <span class="job-name">${escHtml(job.filename || job.id)}</span>
      ${job.status === "printing" ? `
        <div class="progress-bar"><div class="fill" style="width:${pct}%"></div></div>
      ` : ""}
      <span class="job-status ${job.status}">${job.status}</span>
    `;
    return el;
  }

  function escHtml(s) {
    const d = document.createElement("div");
    d.textContent = s;
    return d.innerHTML;
  }
});
