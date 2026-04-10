(function () {
  const out = document.getElementById("out");
  const aux = document.getElementById("aux");
  const promptEl = document.getElementById("prompt");
  const statusEl = document.getElementById("status");
  const sendBtn = document.getElementById("send");

  function setStatus(t) {
    statusEl.textContent = t;
  }

  const es = new EventSource("/ui/sse?session=default");
  es.onopen = function () {
    setStatus("SSE connected");
  };
  es.onerror = function () {
    setStatus("SSE error / reconnecting…");
  };
  es.onmessage = function (ev) {
    let o;
    try {
      o = JSON.parse(ev.data);
    } catch (e) {
      return;
    }
    const k = o.kind;
    if (k === "token" && typeof o.content === "string") {
      out.textContent += o.content;
    } else if (k === "final") {
      out.textContent += "\n[final] " + JSON.stringify(o) + "\n";
    } else if (k === "error") {
      out.textContent += "\n[error] " + (o.message || "") + "\n";
    } else if (k === "aux") {
      aux.textContent += JSON.stringify(o) + "\n";
    }
  };

  sendBtn.addEventListener("click", function () {
    const prompt = promptEl.value.trim();
    if (!prompt) return;
    setStatus("Submitting…");
    sendBtn.disabled = true;
    fetch("/ui/run", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ prompt: prompt }),
    })
      .then(function (r) {
        if (r.status === 202) {
          promptEl.value = "";
          setStatus("Accepted — watch stream above");
        } else {
          return r.text().then(function (t) {
            setStatus("HTTP " + r.status + " " + t);
          });
        }
      })
      .catch(function (e) {
        setStatus("fetch failed: " + e);
      })
      .finally(function () {
        sendBtn.disabled = false;
      });
  });
})();
