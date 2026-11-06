/* Presentación TP ASSD: navegación + interactivos. Todo se dibuja al cargar, así el PDF sale con los gráficos. */
(function () {
  "use strict";
  var $ = function (id) { return document.getElementById(id); };
  var NAVY = "#1c2f57", NAVY2 = "#2f4f86", ACERO = "#7d9cc4", ACENTO = "#b5552f", GRIS = "#9a9a9a", TINTA = "#191919", MUTED = "#5d6168";
  var C = 343, FS = 16000, D0 = 0.0214375;
  var coma = function (x, n) {
    var t = x.toFixed(n === undefined ? 1 : n);
    if (/^-0[.,]?0*$/.test(t)) t = t.slice(1);
    return t.replace(".", ",").replace("-", "−");
  };
  var db = function (x) { return 20 * Math.log10(Math.max(x, 1e-9)); };

  /* ───────── navegación ───────── */
  var slides = [].slice.call(document.querySelectorAll(".slide")), actual = 0;
  function ajustar() {
    var s = Math.min(window.innerWidth / 1920, window.innerHeight / 1080);
    $("stage").style.transform = "scale(" + s + ") translate(-50%, -50%)";
    $("stage").style.transformOrigin = "0 0";
    $("stage").style.left = "50%"; $("stage").style.top = "50%";
    $("stage").style.transform = "translate(-50%, -50%) scale(" + s + ")";
    $("stage").style.transformOrigin = "50% 50%";
  }
  function ir(i) {
    actual = Math.max(0, Math.min(slides.length - 1, i));
    slides.forEach(function (s, k) { s.classList.toggle("activa", k === actual); });
    $("pag").textContent = (actual + 1) + " / " + slides.length;
    if (history.replaceState) history.replaceState(null, "", "#" + (actual + 1));
  }
  document.addEventListener("keydown", function (e) {
    var t = e.target.tagName;
    if ((t === "INPUT" || t === "SELECT") && /Arrow/.test(e.key)) return;
    if (["ArrowRight", "PageDown", " ", "Enter"].indexOf(e.key) >= 0) { e.preventDefault(); ir(actual + 1); }
    else if (["ArrowLeft", "PageUp", "Backspace"].indexOf(e.key) >= 0) { e.preventDefault(); ir(actual - 1); }
    else if (e.key === "Home") ir(0);
    else if (e.key === "End") ir(slides.length - 1);
    else if (e.key === "f" || e.key === "F") { if (!document.fullscreenElement) document.documentElement.requestFullscreen(); else document.exitFullscreen(); }
  });
  // después de mover un control, se le saca el foco para que las flechas vuelvan a pasar slides
  document.addEventListener("change", function (e) { if (e.target.blur) e.target.blur(); });
  document.addEventListener("pointerup", function (e) { if (e.target.type === "range") setTimeout(function () { e.target.blur(); }, 0); });
  $("ant").onclick = function () { ir(actual - 1); };
  $("sig").onclick = function () { ir(actual + 1); };
  window.addEventListener("resize", ajustar);
  ajustar();
  ir((parseInt(location.hash.slice(1), 10) || 1) - 1);

  /* texto de canvas con borde color papel: grillas y curvas no lo cortan (salvo texto blanco sobre fondo oscuro) */
  (function () {
    var C2 = CanvasRenderingContext2D.prototype, fill = C2.fillText;
    C2.fillText = function (t, x, y) {
      var fs = String(this.fillStyle).toLowerCase();
      if (fs !== "#ffffff" && fs !== "#fff" && fs !== "white") {
        this.save(); this.shadowBlur = 0; this.setLineDash([]); this.lineJoin = "round";
        this.strokeStyle = "rgba(240,240,237,0.92)"; this.lineWidth = 5; this.strokeText(t, x, y); this.restore();
      }
      return fill.apply(this, arguments);
    };
  })();

  /* ───────── utilidades de dibujo ───────── */
  function lienzo(cv) {          // resolución ×2 sobre el tamaño de diseño
    if (!cv._w) { cv._w = cv.width; cv._h = cv.height; cv.style.width = cv._w + "px"; cv.style.height = cv._h + "px"; cv.width = cv._w * 2; cv.height = cv._h * 2; }
    var ctx = cv.getContext("2d"); ctx.setTransform(2, 0, 0, 2, 0, 0); ctx.clearRect(0, 0, cv._w, cv._h);
    return ctx;
  }
  function posLocal(cv, e) {
    var r = cv.getBoundingClientRect();
    return [(e.clientX - r.left) / r.width * cv._w, (e.clientY - r.top) / r.height * cv._h];
  }
  var FUENTE = "Barlow, sans-serif";

  /* polar: curvas = [{f(θ) -> valor, color, w, dash, relleno}], θ desde el frente (0) hasta 180°, simétrico */
  function polar(cv, curvas, op) {
    op = op || {};
    var ctx = lienzo(cv), W = cv._w, cx = W / 2, cy = W / 2, R = W / 2 - (op.borde || 40), sep = op.sep || 0, rango = op.rango || 30, esDb = op.escala === "db";
    ctx.lineWidth = 1; ctx.strokeStyle = "#b3b3b3"; ctx.fillStyle = MUTED; ctx.font = "15px " + FUENTE; ctx.textAlign = "center";
    var anillos = esDb ? [0, 10, 20, 30] : [0.25, 0.5, 0.75, 1];
    anillos.forEach(function (a) {
      var r = esDb ? R * (1 - a / rango) : R * a;
      ctx.beginPath(); ctx.arc(cx, cy, Math.max(r, 0.1), 0, 7); ctx.stroke();
      if (esDb && a < rango) ctx.fillText(a === 0 ? "0 dB" : "−" + a, cx + 4 + r * Math.sin(Math.PI / 12), cy - r * Math.cos(Math.PI / 12) + 4);
    });
    for (var g = 0; g < 360; g += 30) { var rg = g * Math.PI / 180; ctx.beginPath(); ctx.moveTo(cx, cy); ctx.lineTo(cx + R * Math.sin(rg), cy - R * Math.cos(rg)); ctx.stroke(); }
    ctx.font = "17px " + FUENTE; ctx.fillStyle = TINTA;
    ctx.fillText("0° frente", cx, cy - R - 12 - sep); ctx.fillText("180°", cx, cy + R + 26 + sep);
    ctx.textAlign = "left"; ctx.fillText("90°", cx + R + 6, cy + 6); ctx.textAlign = "right"; ctx.fillText("90°", cx - R - 6, cy + 6);
    function radio(v) { return esDb ? R * Math.max(0, (db(Math.abs(v)) + rango) / rango) : R * Math.min(Math.abs(v), 1.15); }
    curvas.forEach(function (c) {
      var pts = [];
      for (var j = 0; j <= 360; j++) { var ph = j * Math.PI / 180, th = j <= 180 ? ph : 2 * Math.PI - ph; pts.push([ph, c.f(th)]); }
      var xy = function (p) { var r = radio(p[1]); return [cx + r * Math.sin(p[0]), cy - r * Math.cos(p[0])]; };
      if (c.relleno) { ctx.beginPath(); pts.forEach(function (p, i) { var q = xy(p); i ? ctx.lineTo(q[0], q[1]) : ctx.moveTo(q[0], q[1]); }); ctx.closePath(); ctx.globalAlpha = 0.12; ctx.fillStyle = c.color; ctx.fill(); ctx.globalAlpha = 1; }
      ctx.lineWidth = c.w || 3;
      for (var i = 1; i < pts.length; i++) {
        var neg = !esDb && (pts[i - 1][1] + pts[i][1]) / 2 < -1e-6;
        ctx.setLineDash(neg ? [7, 6] : (c.dash || [])); ctx.strokeStyle = neg ? ACENTO : c.color;
        var a = xy(pts[i - 1]), b = xy(pts[i]); ctx.beginPath(); ctx.moveTo(a[0], a[1]); ctx.lineTo(b[0], b[1]); ctx.stroke();
      }
      ctx.setLineDash([]);
    });
    if (op.puntos) op.puntos.forEach(function (p) {
      var ph = p.th * Math.PI / 180, r = R * Math.max(0, (p.db + rango) / rango), x = cx + r * Math.sin(ph), y = cy - r * Math.cos(ph);
      ctx.beginPath(); ctx.arc(x, y, p.r || 9, 0, 7); ctx.fillStyle = p.hueco ? "#f4f4f2" : p.color; ctx.fill(); ctx.lineWidth = 3; ctx.strokeStyle = p.color; ctx.stroke();
      if (p.txt) { var izq = p.lado ? p.lado === "izq" : x > cx; ctx.fillStyle = p.color; ctx.font = "600 17px " + FUENTE; ctx.textAlign = izq ? "right" : "left"; ctx.fillText(p.txt, x + (izq ? -16 : 16), y - 12); }
    });
    if (op.marca) {
      var m = op.marca, phm = m.ph;
      ctx.strokeStyle = TINTA; ctx.lineWidth = 1.5; ctx.setLineDash([4, 4]);
      ctx.beginPath(); ctx.moveTo(cx, cy); ctx.lineTo(cx + R * Math.sin(phm), cy - R * Math.cos(phm)); ctx.stroke(); ctx.setLineDash([]);
      ctx.font = "600 18px " + FUENTE; ctx.textAlign = "left";
      m.lineas.forEach(function (l, k) { ctx.fillStyle = l[1]; ctx.fillText(l[0], 14, 24 + k * 24); });
    }
    return { cx: cx, cy: cy, R: R };
  }
  function anguloDesde(cv, e) {
    var q = posLocal(cv, e), cx = cv._w / 2, cy = cv._w / 2, ph = Math.atan2(q[0] - cx, -(q[1] - cy));
    if (ph < 0) ph += 2 * Math.PI;
    return { ph: ph, th: ph <= Math.PI ? ph : 2 * Math.PI - ph };
  }
  function df(fn) {             // DF = |H(0)|² / (½∫|H|² sinθ dθ), integración trapezoidal
    var N = 720, den = 0, h0 = Math.abs(fn(0));
    for (var i = 0; i <= N; i++) { var th = Math.PI * i / N, v = Math.abs(fn(th)); den += ((i === 0 || i === N) ? 0.5 : 1) * v * v * Math.sin(th) * Math.PI / N; }
    return 10 * Math.log10(h0 * h0 / (0.5 * den));
  }
  function frec(v) { return 100 * Math.pow(80, v); }
  function fmtF(f) { return f >= 1000 ? coma(f / 1000, 2) + " kHz" : coma(f, 0) + " Hz"; }

  /* modelos del array (respuesta exacta) */
  function cardioides(th, f, b, d) {    // c_F = 1 − x2·z, c_B = x2 − z, con x2 = e^{-jωτ0cosθ}, z = e^{-jωT}
    var w = 2 * Math.PI * f, u = w * d / C * Math.cos(th), v = w / FS;
    var xr = Math.cos(u), xi = -Math.sin(u), zr = Math.cos(v), zi = -Math.sin(v);
    var fr = 1 - (xr * zr - xi * zi), fi = -(xr * zi + xi * zr), br = xr - zr, bi = xi - zi;
    return { cf: Math.hypot(fr, fi), cb: Math.hypot(br, bi), y: Math.hypot(fr - b * br, fi - b * bi) };
  }

  /* ───────── 7. comparación suma contra resta ───────── */
  function iniComparacion() {
    var cv = $("cmp-polar"), marca = null;
    var curvas = [
      { id: "dsb-b", nom: "Delay-and-sum broadside", color: GRIS, dash: [10, 7], on: true, f: function (th, f, d) { return Math.abs(Math.cos(Math.PI * f * d / C * Math.cos(th))); } },
      { id: "dsb-e", nom: "Delay-and-sum endfire", color: ACERO, on: true, f: function (th, f, d) { return Math.abs(Math.cos(Math.PI * f * d / C * (Math.cos(th) - 1))); } },
      { id: "card", nom: "Cardioide (resta)", color: NAVY2, dash: [4, 5], on: false, f: function (th, f, d) { return cardioides(th, f, 0, d).cf; } },
      { id: "hip", nom: "Hipercardioide (resta)", color: NAVY, on: true, w: 4.5, f: function (th, f, d) { return cardioides(th, f, 0.5, d).y; } }
    ];
    $("cmp-ley").innerHTML = curvas.map(function (c) {
      return '<label><input type="checkbox" data-id="' + c.id + '"' + (c.on ? " checked" : "") + '><i style="border-color:' + c.color + (c.dash ? ';border-top-style:dashed' : '') + '"></i>' + c.nom + "</label>";
    }).join("");
    $("cmp-ley").addEventListener("change", function (e) { curvas.forEach(function (c) { if (c.id === e.target.dataset.id) c.on = e.target.checked; }); dibujar(); });
    function dibujar() {
      var f = frec(+$("cmp-f").value), d = +$("cmp-d").value / 1000;
      $("cmp-f-o").textContent = fmtF(f);
      $("cmp-d-o").textContent = coma(d * 1000, 1) + " mm";
      var act = curvas.filter(function (c) { return c.on; });
      var norm = act.map(function (c) { var h0 = Math.max(c.f(0, f, d), 1e-9); return function (th) { return c.f(th, f, d) / h0; }; });
      var op = { escala: "db", rango: 30 };
      if (marca) op.marca = { ph: marca.ph, lineas: [["θ = " + coma(marca.th * 180 / Math.PI, 0) + "°", TINTA]].concat(act.map(function (c, k) { return [coma(db(Math.abs(norm[k](marca.th))), 1) + " dB  " + c.nom.split(" (")[0], c.color]; })) };
      polar(cv, act.map(function (c, k) { return { f: norm[k], color: c.color, w: c.w || 3, dash: c.dash, relleno: c.id === "hip" }; }), op);
      $("cmp-tab").innerHTML = "<tr><th>Algoritmo</th><th>DF</th><th>A 90°</th></tr>" + curvas.map(function (c) {
        var fn = function (th) { return c.f(th, f, d); };
        return "<tr" + (c.id === "hip" ? ' class="dest"' : "") + "><td>" + c.nom.split(" (")[0] + "</td><td>" + coma(df(fn), 1) + " dB</td><td>" + coma(db(fn(Math.PI / 2) / Math.max(fn(0), 1e-9)), 1) + " dB</td></tr>";
      }).join("");
    }
    cv.addEventListener("mousemove", function (e) { marca = anguloDesde(cv, e); dibujar(); });
    cv.addEventListener("mouseleave", function () { marca = null; dibujar(); });
    ["cmp-f", "cmp-d"].forEach(function (id) { $(id).addEventListener("input", dibujar); });
    dibujar();
  }

  /* ───────── 11. dos cardioides → hipercardioide ───────── */
  function iniHiper() {
    var marca = null;
    function partes(th, modo, f, b) {
      if (modo === "ideal") { var c = Math.cos(th), cf = (1 + c) / 2, cb = (1 - c) / 2; return { cf: cf, cb: cb, y: cf - b * cb }; }
      return cardioides(th, f, b, D0);
    }
    function dibujar() {
      var b = +$("h-b").value, modo = $("h-modo").value, f = frec(+$("h-f").value);
      $("h-b-o").textContent = coma(b, 2); $("h-f-o").textContent = fmtF(f);
      $("h-f-w").style.visibility = modo === "exacto" ? "visible" : "hidden";
      $("h-cb-t").innerHTML = coma(b, 2) + " · c<sub>B</sub>";
      [].forEach.call(document.querySelectorAll("#h-pre button"), function (bt) { bt.setAttribute("aria-pressed", Math.abs(+bt.dataset.b - b) < 0.005); });
      var esc = modo === "ideal" ? 1 : Math.max(partes(0, modo, f, 0).cf, 1e-9);
      var g = function (k, m) { return function (th) { return (m || 1) * partes(th, modo, f, b)[k] / esc; }; };
      polar($("h-cf"), [{ f: g("cf"), color: NAVY2, relleno: true }]);
      polar($("h-cb"), [{ f: function (th) { return b * partes(th, modo, f, b).cb / esc; }, color: ACENTO, relleno: true }]);
      var yN = g("y"), front = Math.abs(yN(0)), op = {};
      var nombre = b < 0.02 ? "cardioide" : Math.abs(b - 0.27) < 0.03 ? "supercardioide" : Math.abs(b - 0.5) < 0.03 ? "hipercardioide" : b > 0.98 ? "dipolo" : "intermedio";
      $("h-y-t").textContent = "y · " + nombre;
      if (marca) op.marca = { ph: marca.ph, lineas: [["θ = " + coma(marca.th * 180 / Math.PI, 0) + "°", TINTA], [coma(db(Math.abs(yN(marca.th)) / front), 1) + " dB", NAVY]] };
      polar($("h-y"), [{ f: yN, color: NAVY, w: 4, relleno: true }], op);
      var N = 720, mn = 1e9, mnT = 0;
      for (var i = 0; i <= N; i++) { var th = Math.PI * i / N, v = Math.abs(yN(th)); if (v < mn) { mn = v; mnT = th; } }
      var rel = function (th) { return coma(db(Math.abs(yN(th)) / front), 1) + " dB"; };
      $("h-lect").innerHTML = "<dt>Costado (90°)</dt><dd>" + rel(Math.PI / 2) + "</dd><dt>Atrás (180°)</dt><dd>" + rel(Math.PI) +
        "</dd><dt>Nulo</dt><dd>" + (mn / front < 0.05 ? coma(mnT * 180 / Math.PI, 0) + "°" : "sin nulo") + "</dd><dt>DF</dt><dd>" + coma(df(yN), 1) + " dB</dd>";
    }
    [].forEach.call(document.querySelectorAll("#h-pre button"), function (bt) { bt.onclick = function () { $("h-b").value = bt.dataset.b; dibujar(); bt.blur(); }; });
    ["h-b", "h-modo", "h-f"].forEach(function (id) { $(id).addEventListener("input", dibujar); });
    $("h-y").addEventListener("mousemove", function (e) { marca = anguloDesde($("h-y"), e); dibujar(); });
    $("h-y").addEventListener("mouseleave", function () { marca = null; dibujar(); });
    dibujar();
  }

  /* ───────── 11b y 12. respuesta en frecuencia del hipercardioide: sin integrador y compensada ───────── */
  function iniPrecio() {
    respuesta($("pr-resp"), false);
    respuesta($("eq-resp"), true);
  }
  function respuesta(cv, comp) {
    if (!cv) return;
    var lg = function (f) { return Math.log10(f); }, FMAX = 6000, A = Math.exp(-2 * Math.PI * 150 / FS);   // fc = 150 Hz, igual que ORBITA_BF_FC_HZ
    // integrador con pérdida y ÷2: H = 0,5 / (1 − a·z⁻¹)
    var integ = function (f) { var w = 2 * Math.PI * f / FS; return 0.5 / Math.hypot(1 - A * Math.cos(w), A * Math.sin(w)); };
    var yMin = comp ? -30 : -40, yMax = comp ? 6 : 10;
    var g = grafico(cv, { x: [lg(100), lg(FMAX)], y: [yMin, yMax], yt: comp ? [-30, -20, -10, 0] : [-40, -30, -20, -10, 0, 10], yfmt: function (t) { return coma(t, 0); },
      xt: [100, 200, 500, 1000, 2000, 4000].map(lg), xfmt: function (t) { var f = Math.round(Math.pow(10, t)); return f >= 1000 ? (f / 1000) + " kHz" : f + " Hz"; },
      titulo: comp ? "|y| en dB respecto de un solo mic · β = 0,5 · con integrador" : "|y| en dB respecto de un solo mic · β = 0,5 · sin integrador", margen: [70, 30, 56, 50] });
    var ctx = g.ctx, N = 240, fs = [];
    for (var i = 0; i <= N; i++) fs.push(100 * Math.pow(FMAX / 100, i / N));
    linea(g, [lg(100), lg(FMAX)], [0, 0], GRIS, 1.5, [6, 6]);
    ctx.font = "16px " + FUENTE; ctx.fillStyle = MUTED; ctx.textAlign = "left"; ctx.fillText("un solo mic (0 dB)", g.X(lg(100)) + 10, g.Y(0) - 8);
    var curvas = [[0, NAVY, 4.5, "0° (frente)"], [90, ACERO, 3.5, "90° (costado)"], [180, ACENTO, 3.5, "180° (atrás)"]];
    curvas.forEach(function (c) {
      linea(g, fs.map(lg), fs.map(function (f) { return Math.max(db(cardioides(c[0] * Math.PI / 180, f, 0.5, D0).y * (comp ? integ(f) : 1)), yMin); }), c[1], c[2]);
    });
    ctx.font = "600 17px " + FUENTE; ctx.textAlign = "left";
    curvas.forEach(function (c, k) {
      var x = g.X(lg(comp ? 160 : 1300)), y = g.Y(comp ? -22 : -24) + k * 28;
      ctx.strokeStyle = c[1]; ctx.lineWidth = 4; ctx.beginPath(); ctx.moveTo(x, y - 5); ctx.lineTo(x + 40, y - 5); ctx.stroke();
      ctx.fillStyle = c[1] === ACERO ? NAVY2 : c[1]; ctx.fillText(c[3], x + 52, y);
    });
    ctx.font = "600 18px " + FUENTE; ctx.fillStyle = NAVY; ctx.textAlign = "left";
    if (!comp) ctx.fillText("+6 dB/oct", g.X(lg(210)), g.Y(-8));
    else { ctx.fillText("plano de ~300 Hz a 3 kHz", g.X(lg(420)), g.Y(0) + 34); ctx.fillText("−12 dB", g.X(lg(1500)), g.Y(-12) - 10); ctx.fillText("−6 dB", g.X(lg(1500)), g.Y(-6) - 10); }
  }

  /* ───────── 14. medición en placa ───────── */
  function iniMedicion() {
    var B = function (th) { return 0.25 + 0.75 * Math.cos(th); };
    /* [ángulo donde estaba el parlante, compensado, sin compensar, nombre]; arriba a la izquierda y costado a la derecha (los dos a 90° del eje) */
    var med = [[0, -0.7, -3.0, "Frente"], [-90, -7.1, -3.8, "Arriba"], [90, -15.1, -3.8, "Costado"], [180, -3.0, 1.7, "Atrás"]];
    var pts = [];
    med.forEach(function (m) { pts.push({ th: m[0], db: m[2], color: "#8a8a8a", hueco: true, r: 8 }); });
    med.forEach(function (m) { pts.push({ th: m[0], db: m[1], color: NAVY, r: 10, txt: m[3] + " " + coma(m[1], 1) + " dB", lado: m[0] === 0 ? "izq" : null }); });
    polar($("med-polar"), [{ f: B, color: ACERO, w: 3, relleno: true }], { escala: "db", rango: 30, puntos: pts, borde: 60, sep: 20 });
    var ctx = $("med-polar").getContext("2d"); ctx.font = "17px " + FUENTE; ctx.textAlign = "left";
    [["teoría (baja frecuencia)", ACERO], ["medido, compensado", NAVY], ["medido, sin compensar", "#8a8a8a"]].forEach(function (l, k) { ctx.fillStyle = l[1]; ctx.fillText("● " + l[0], 10, 600 + k * 24); });
  }

  /* ───────── base64 ───────── */
  function b64(s, Tipo) { var bin = atob(s), u = new Uint8Array(bin.length); for (var i = 0; i < bin.length; i++) u[i] = bin.charCodeAt(i); return new Tipo(u.buffer); }
  function mapa(v) {            // 0..1 → papel → acero → azul marino
    var paradas = [[0, [244, 241, 234]], [0.35, [125, 156, 196]], [0.7, [28, 47, 87]], [1, [10, 18, 36]]];
    for (var i = 1; i < paradas.length; i++) if (v <= paradas[i][0]) {
      var a = paradas[i - 1], b = paradas[i], t = (v - a[0]) / (b[0] - a[0]);
      return [0, 1, 2].map(function (k) { return Math.round(a[1][k] + t * (b[1][k] - a[1][k])); });
    }
    return paradas[3][1];
  }
  function imagenFeat(feat, cols, desde, hasta, maxv) {   // ImageData de 40 filas (grave abajo)
    var n = hasta - desde, cv = document.createElement("canvas"); cv.width = n; cv.height = 40;
    var ctx = cv.getContext("2d"), im = ctx.createImageData(n, 40);
    for (var c = 0; c < n; c++) for (var b = 0; b < 40; b++) {
      var v = Math.min(feat[(desde + c) * 40 + b] / maxv, 1), rgb = mapa(v), o = ((39 - b) * n + c) * 4;
      im.data[o] = rgb[0]; im.data[o + 1] = rgb[1]; im.data[o + 2] = rgb[2]; im.data[o + 3] = 255;
    }
    ctx.putImageData(im, 0, 0); return cv;
  }

  /* ───────── 21. espectrograma ───────── */
  function fft(re, im) {        // radix-2 in place
    var n = re.length;
    for (var i = 1, j = 0; i < n; i++) { var bit = n >> 1; for (; j & bit; bit >>= 1) j ^= bit; j ^= bit; if (i < j) { var t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; } }
    for (var len = 2; len <= n; len <<= 1) {
      var ang = -2 * Math.PI / len, wr = Math.cos(ang), wi = Math.sin(ang);
      for (var s = 0; s < n; s += len) { var cr = 1, ci = 0;
        for (var k = 0; k < len / 2; k++) { var a = s + k, b2 = a + len / 2, xr = re[b2] * cr - im[b2] * ci, xi = re[b2] * ci + im[b2] * cr;
          re[b2] = re[a] - xr; im[b2] = im[a] - xi; re[a] += xr; im[a] += xi; var nr = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = nr; } }
    }
  }
  function iniEspectro() {
    var E = window.DATOS.espectro, x = b64(E.muestras, Int16Array), feat = b64(E.feat, Uint8Array), cols = E.cols, L = 480, H = 160, N = 512;
    var maxF = 0; for (var i = 0; i < feat.length; i++) maxF = Math.max(maxF, feat[i]);
    var hann = new Float32Array(L); for (i = 0; i < L; i++) hann[i] = 0.5 * (1 - Math.cos(2 * Math.PI * i / (L - 1)));
    var mel = function (f) { return 1127 * Math.log(1 + f / 700); }, imel = function (m) { return 700 * (Math.exp(m / 1127) - 1); };
    var bordes = []; for (i = 0; i <= 41; i++) bordes.push(imel(mel(125) + (mel(7500) - mel(125)) * i / 41));
    var imgSpec = imagenFeat(feat, cols, 0, cols, maxF), xmax = 0; for (i = 0; i < x.length; i++) xmax = Math.max(xmax, Math.abs(x[i]));
    $("e-m").max = cols - 1;
    // forma de onda fija (min/max por píxel)
    function onda(m) {
      var cv = $("e-onda"), ctx = lienzo(cv), W = cv._w, Hh = cv._h, mid = Hh / 2, por = x.length / W;
      ctx.strokeStyle = "#b9b9b9"; ctx.beginPath(); ctx.moveTo(0, mid); ctx.lineTo(W, mid); ctx.stroke();
      ctx.fillStyle = NAVY2;
      for (var px = 0; px < W; px++) { var a = Math.floor(px * por), b = Math.floor((px + 1) * por), mn = 0, mx = 0;
        for (var k = a; k < b; k++) { if (x[k] < mn) mn = x[k]; if (x[k] > mx) mx = x[k]; }
        ctx.fillRect(px, mid - mx / xmax * (mid - 8), 1, Math.max(1, (mx - mn) / xmax * (mid - 8))); }
      var x0 = m * H / x.length * W, w = L / x.length * W;
      ctx.fillStyle = "rgba(181,85,47,.16)"; ctx.fillRect(x0, 4, w, Hh - 32);
      ctx.strokeStyle = ACENTO; ctx.lineWidth = 2.5; ctx.strokeRect(x0, 4, w, Hh - 32);
      ctx.beginPath(); for (var j = 0; j < L; j += 4) { var yy = Hh - 30 - hann[j] * (Hh - 38); j ? ctx.lineTo(x0 + j / L * w, yy) : ctx.moveTo(x0, yy); } ctx.stroke();
      ctx.fillStyle = MUTED; ctx.font = "15px " + FUENTE; ctx.textAlign = "left";
      for (var t = 0; t <= 1.5; t += 0.25) ctx.fillText(coma(t, 2) + " s", t / 1.5 * W + 3, Hh - 6);
    }
    function dibujar() {
      var m = +$("e-m").value, ini = m * H;
      $("e-m-o").textContent = m + " (" + coma(ini / 16, 0) + "–" + coma((ini + L) / 16, 0) + " ms)";
      onda(m);
      // trama
      var re = new Float32Array(N), im = new Float32Array(N);
      for (var n = 0; n < L; n++) re[n] = (x[ini + n] || 0) * hann[n] / 32768;
      var cv = $("e-trama"), ctx = lienzo(cv), W = cv._w, Hh = cv._h, mid = Hh / 2, pk = 1e-6;
      for (n = 0; n < L; n++) pk = Math.max(pk, Math.abs(re[n]));
      ctx.strokeStyle = "#b9b9b9"; ctx.lineWidth = 1; ctx.beginPath(); ctx.moveTo(0, mid); ctx.lineTo(W, mid); ctx.stroke();
      ctx.strokeStyle = ACENTO; ctx.setLineDash([5, 5]); ctx.beginPath();
      for (n = 0; n < L; n++) { var yy = mid - hann[n] * (mid - 10); n ? ctx.lineTo(n / L * W, yy) : ctx.moveTo(0, yy); } ctx.stroke();
      ctx.beginPath(); for (n = 0; n < L; n++) { var yn = mid + hann[n] * (mid - 10); n ? ctx.lineTo(n / L * W, yn) : ctx.moveTo(0, yn); } ctx.stroke(); ctx.setLineDash([]);
      ctx.strokeStyle = NAVY; ctx.lineWidth = 1.6; ctx.beginPath();
      for (n = 0; n < L; n++) { var y2 = mid - re[n] / pk * (mid - 10); n ? ctx.lineTo(n / L * W, y2) : ctx.moveTo(0, y2); } ctx.stroke();
      ctx.fillStyle = MUTED; ctx.font = "15px " + FUENTE; ctx.fillText("0", 2, Hh - 4); ctx.textAlign = "right"; ctx.fillText("30 ms", W - 2, Hh - 4); ctx.textAlign = "left";
      // FFT y filtros
      fft(re, im);
      var mag = new Float32Array(N / 2 + 1); for (var k = 0; k <= N / 2; k++) mag[k] = 20 * Math.log10(Math.hypot(re[k], im[k]) + 1e-7);
      cv = $("e-fft"); ctx = lienzo(cv); W = cv._w; Hh = cv._h;
      var fx = function (f) { return f / 8000 * W; }, top = 10, alto = Hh - 34, dbMin = -50, dbMax = 45;
      var fy = function (d) { return top + (1 - (Math.max(dbMin, d) - dbMin) / (dbMax - dbMin)) * alto; };
      ctx.lineWidth = 1;
      for (var b = 0; b < 40; b++) { var a0 = fx(bordes[b]), a1 = fx(bordes[b + 1]), a2 = fx(bordes[b + 2]);
        ctx.strokeStyle = b % 2 ? "rgba(47,79,134,.35)" : "rgba(181,85,47,.35)"; ctx.beginPath(); ctx.moveTo(a0, top + alto); ctx.lineTo(a1, top + 40); ctx.lineTo(a2, top + alto); ctx.stroke(); }
      ctx.strokeStyle = NAVY; ctx.lineWidth = 2; ctx.beginPath();
      for (k = 0; k <= N / 2; k++) { var xx = fx(k * FS / N), y3 = fy(mag[k]); k ? ctx.lineTo(xx, y3) : ctx.moveTo(xx, y3); } ctx.stroke();
      ctx.fillStyle = MUTED; ctx.font = "15px " + FUENTE;
      [0, 1000, 2000, 4000, 6000, 8000].forEach(function (f) { ctx.textAlign = f === 8000 ? "right" : "left"; ctx.fillText(f / 1000 + " kHz", fx(f) + (f ? 0 : 2), Hh - 6); });
      // columna real
      cv = $("e-col"); ctx = lienzo(cv); W = cv._w; Hh = cv._h;
      var hc = (Hh - 10) / 40;
      for (b = 0; b < 40; b++) { var rgb = mapa(Math.min(feat[m * 40 + b] / maxF, 1)); ctx.fillStyle = "rgb(" + rgb + ")"; ctx.fillRect(20, Hh - 5 - (b + 1) * hc, W - 40, hc + 0.5); }
      ctx.strokeStyle = ACENTO; ctx.lineWidth = 2.5; ctx.strokeRect(20, 5, W - 40, Hh - 10);
      // espectrograma que se va armando
      cv = $("e-spec"); ctx = lienzo(cv); W = cv._w; Hh = cv._h;
      ctx.imageSmoothingEnabled = false; ctx.drawImage(imgSpec, 0, 0, cols, 40, 0, 0, W, Hh - 22);
      var cw = W / cols;
      ctx.fillStyle = "rgba(239,239,237,.82)"; ctx.fillRect((m + 1) * cw, 0, W - (m + 1) * cw, Hh - 22);
      ctx.strokeStyle = ACENTO; ctx.lineWidth = 2.5; ctx.strokeRect(m * cw, 1, cw, Hh - 24);
      ctx.fillStyle = MUTED; ctx.font = "15px " + FUENTE;
      for (var t = 0; t < cols; t += 25) ctx.fillText(t, t * cw + 2, Hh - 4);
      ctx.textAlign = "right"; ctx.fillText("columna", W, Hh - 4);
    }
    var timer = null;
    $("e-play").onclick = function () {
      if (timer) { clearInterval(timer); timer = null; this.textContent = "▶ Reproducir"; this.classList.remove("on"); return; }
      var bt = this; bt.textContent = "■ Pausa"; bt.classList.add("on");
      if (+$("e-m").value >= cols - 1) $("e-m").value = 0;
      timer = setInterval(function () { var v = +$("e-m").value + 1; if (v >= cols) { clearInterval(timer); timer = null; bt.textContent = "▶ Reproducir"; bt.classList.remove("on"); return; } $("e-m").value = v; dibujar(); }, 45);
      bt.blur();
    };
    $("e-m").addEventListener("input", dibujar);
    dibujar();
  }

  /* ───────── 22. la red ───────── */
  /* ───────── etapas de la red (forma de cada tensor, al estilo de un diagrama de CNN) ───────── */
  function iniEtapas() {
    var cv = $("r-etapas"); if (!cv) return;
    var ctx = lienzo(cv), W = cv._w;
    var D = window.DATOS.deteccion, feat = b64(D.feat, Uint8Array), finOrb = Math.round((D.marcas.orbita + 0.75) * 100);
    var maxF = 0; for (var i = 0; i < feat.length; i++) maxF = Math.max(maxF, feat[i]);
    var img = imagenFeat(feat, D.cols, finOrb - 170, finOrb, maxF);
    var SX = 2.8, OB = [26, -18], Y0 = 330, YN = Y0 - alto(64) - 18 - 52;                 // px por columna; desplazamiento oblicuo (profundidad)
    var et = [
      { nom: "Conv 1", det: "k = 5, p = 3", T: 56, C: 32, k: 5 },
      { nom: "Bloque 1", det: "k = 5, p = 1", T: 52, C: 64, k: 5 },
      { nom: "Bloque 2", det: "k = 7 y 11, p = 1", T: 42, C: 64, k: 11 },
      { nom: "Bloque 3", det: "k = 9 y 15, p = 1", T: 28, C: 64, k: 15 },
      { nom: "Bloque 4", det: "k = 23, p = 1", T: 6, C: 64, k: 23 }
    ];
    function alto(C) { return C * 2.8; }
    function caja(x, w, h, tono) {                        // frente apoyado en Y0, cara superior y lateral oblicuas
      var y = Y0 - h, dx = OB[0], dy = OB[1];
      ctx.lineWidth = 2; ctx.strokeStyle = NAVY; ctx.lineJoin = "round";
      ctx.beginPath(); ctx.moveTo(x, y); ctx.lineTo(x + dx, y + dy); ctx.lineTo(x + w + dx, y + dy); ctx.lineTo(x + w, y); ctx.closePath();
      ctx.fillStyle = tono[1]; ctx.fill(); ctx.stroke();
      ctx.beginPath(); ctx.moveTo(x + w, y); ctx.lineTo(x + w + dx, y + dy); ctx.lineTo(x + w + dx, Y0 + dy); ctx.lineTo(x + w, Y0); ctx.closePath();
      ctx.fillStyle = tono[2]; ctx.fill(); ctx.stroke();
      ctx.beginPath(); ctx.rect(x, y, w, h); ctx.fillStyle = tono[0]; ctx.fill(); ctx.stroke();
    }
    function cuña(xa, wa, ha, xb, hb, wb) {                   // ventana del kernel sobre la etapa anterior -> un punto de la siguiente
      ctx.save(); ctx.strokeStyle = ACENTO; ctx.lineWidth = 1.6; ctx.setLineDash([5, 4]);
      var y0 = Y0 - ha, bx = xb + 6, by = Y0 - hb / 2;
      ctx.strokeRect(xa, y0, wa, ha);
      ctx.beginPath(); ctx.moveTo(xa + wa, y0); ctx.lineTo(bx, by); ctx.moveTo(xa + wa, Y0); ctx.lineTo(bx, by); ctx.stroke();
      ctx.setLineDash([]); ctx.fillStyle = ACENTO; ctx.beginPath(); ctx.arc(bx, by, 4, 0, 7); ctx.fill(); ctx.restore();
    }
    function rotulo(xc, nom, det, forma, h) {
      ctx.textAlign = "center";
      ctx.font = "600 21px " + FUENTE; ctx.fillStyle = TINTA; ctx.fillText(nom, xc, YN);
      ctx.font = "18px " + FUENTE; ctx.fillStyle = MUTED; ctx.fillText(det, xc, YN + 26);
      ctx.font = "500 20px " + FUENTE; ctx.fillStyle = NAVY; ctx.fillText(forma, xc, Y0 + 40);
    }
    var GAP = 100, x = 20;
    // entrada: espectrograma real (170 columnas × 40 bandas)
    var wIn = 170 * 2.24, hIn = 40 * 3.6;
    ctx.imageSmoothingEnabled = false; ctx.drawImage(img, x, Y0 - hIn, wIn, hIn);
    ctx.lineWidth = 2; ctx.strokeStyle = NAVY; ctx.strokeRect(x, Y0 - hIn, wIn, hIn);
    rotulo(x + wIn / 2, "Entrada", "espectrograma de “Órbita”, 1,7 s", "170 × 40", hIn);
    var prev = { x: x, w: wIn, h: hIn }; x += wIn + GAP;
    et.forEach(function (e, k) {
      var w = e.T * SX, h = alto(e.C), kw = Math.min(prev.w, (k === 0 ? 5 : e.k) * SX * (k === 0 ? 1 : 1));
      cuña(prev.x + prev.w - kw, kw, prev.h, x, h, w);
      caja(x, w, h, ["#dfe6f1", "#f4f6fa", "#b9c7dc"]);
      rotulo(x + w / 2 + OB[0] / 2, e.nom, e.det, e.T + " × " + e.C, h);
      prev = { x: x, w: w, h: h }; x += w + GAP;
    });
    // salida: flatten + dense + sigmoide
    var xs = prev.x + prev.w + 190, ys = Y0 - alto(64) / 2;
    ctx.strokeStyle = NAVY; ctx.lineWidth = 2.6; ctx.beginPath(); ctx.moveTo(prev.x + prev.w + OB[0] + 8, ys); ctx.lineTo(xs - 34, ys); ctx.stroke();
    ctx.fillStyle = NAVY; ctx.beginPath(); ctx.moveTo(xs - 26, ys); ctx.lineTo(xs - 40, ys - 8); ctx.lineTo(xs - 40, ys + 8); ctx.fill();
    ctx.beginPath(); ctx.arc(xs, ys, 24, 0, 7); ctx.fillStyle = "#f4f6fa"; ctx.fill(); ctx.stroke();
    ctx.font = "italic 600 24px " + FUENTE; ctx.fillStyle = NAVY; ctx.textAlign = "center"; ctx.fillText("σ", xs, ys + 8);
    ctx.font = "600 21px " + FUENTE; ctx.fillStyle = TINTA; ctx.fillText("Salida", xs, YN);
    ctx.font = "18px " + FUENTE; ctx.fillStyle = MUTED; ctx.fillText("un número de 0 a 1", xs, YN + 26);
    ctx.font = "500 20px " + FUENTE; ctx.fillStyle = NAVY; ctx.fillText("1", xs, Y0 + 40);
    ctx.font = "600 22px " + FUENTE; ctx.fillStyle = ACENTO; ctx.textAlign = "left"; ctx.fillText("“Órbita”", xs + 36, ys + 8);
    // eje de tiempo
    ctx.font = "17px " + FUENTE; ctx.fillStyle = MUTED; ctx.textAlign = "left";
    ctx.fillText("tiempo →  (ancho de cada bloque = pasos de tiempo; alto = canales)", 20, Y0 + 84);
  }

  function iniRed() {
    var capas = [
      { nom: "Entrada", k: 1, s: 1, info: "40 bandas, una columna cada 10 ms.", pesos: 0 },
      { nom: "Conv 1", tipo: "convolución", k: 5, s: 3, info: "32 filtros de 5 columnas × 40 bandas, paso 3, ReLU.", pesos: 6400 },
      { nom: "Bloque 1", tipo: "depthwise", k: 5, s: 1, info: "Depthwise k = 5 + pointwise 32 → 64, BatchNorm, ReLU.", pesos: 160 + 2048 },
      { nom: "Bloque 2", tipo: "MixConv", k: 11, s: 1, info: "MixConv k = 7 y 11 (mitad de los canales cada uno) + pointwise 64 → 64.", pesos: 576 + 4096 },
      { nom: "Bloque 3", tipo: "MixConv", k: 15, s: 1, info: "MixConv k = 9 y 15 + pointwise 64 → 64.", pesos: 768 + 4096 },
      { nom: "Bloque 4", tipo: "depthwise", k: 23, s: 1, info: "Depthwise k = 23 + pointwise 64 → 64.", pesos: 1472 + 4096 },
      { nom: "Salida", k: 0, s: 1, info: "Flatten de los últimos pasos → Dense de 1 neurona → sigmoide = probabilidad.", pesos: null }
    ];
    var n = [170]; for (var l = 1; l < 6; l++) n.push(Math.floor((n[l - 1] - capas[l].k) / capas[l].s) + 1); n.push(1);
    var D = window.DATOS.deteccion, feat = b64(D.feat, Uint8Array), finOrb = Math.round((D.marcas.orbita + 0.75) * 100);
    var maxF = 0; for (var i = 0; i < feat.length; i++) maxF = Math.max(maxF, feat[i]);
    var img = imagenFeat(feat, D.cols, finOrb - 170, finOrb, maxF);
    var cv = $("r-red"), W = 1080, Hh = 640, x0 = 150, x1 = W - 20, ys = [585, 470, 385, 300, 215, 130, 50];
    var sel = { l: 5, i: n[5] - 1 };
    function xNodo(l, i) { return x0 + (x1 - x0) * (n[l] === 1 ? 0.5 : (i + 0.5) / n[l]); }
    function campo(l, i) {            // rango de nodos por capa, de l hacia abajo
      var r = []; r[l] = [i, i];
      if (l === 6) r[5] = [0, n[5] - 1];
      for (var k = Math.min(l, 5); k >= 1; k--) { var a = r[k][0], b = r[k][1]; r[k - 1] = [a * capas[k].s, b * capas[k].s + capas[k].k - 1]; }
      return r;
    }
    function dibujar() {
      var ctx = lienzo(cv), r = campo(sel.l, sel.i);
      // cono del campo receptivo
      for (var l = 0; l < sel.l; l++) {
        var a = r[l], b = r[l + 1]; if (!a || !b) continue;
        var w0 = (x1 - x0) / n[l], w1 = (x1 - x0) / n[l + 1];
        ctx.fillStyle = "rgba(47,79,134,.13)"; ctx.beginPath();
        ctx.moveTo(xNodo(l, a[0]) - w0 / 2, ys[l]); ctx.lineTo(xNodo(l, a[1]) + w0 / 2, ys[l]);
        ctx.lineTo(xNodo(l + 1, b[1]) + (n[l + 1] === 1 ? 6 : w1 / 2), ys[l + 1]); ctx.lineTo(xNodo(l + 1, b[0]) - (n[l + 1] === 1 ? 6 : w1 / 2), ys[l + 1]); ctx.fill();
      }
      // entrada: el espectrograma real de "Órbita"
      ctx.imageSmoothingEnabled = false; ctx.drawImage(img, x0, ys[0] - 30, x1 - x0, 60);
      ctx.fillStyle = "rgba(239,239,237,.55)";
      var a0 = r[0] || [0, -1];
      ctx.fillRect(x0, ys[0] - 30, (a0[0]) / 170 * (x1 - x0), 60); ctx.fillRect(x0 + (a0[1] + 1) / 170 * (x1 - x0), ys[0] - 30, (169 - a0[1]) / 170 * (x1 - x0), 60);
      if (r[0]) { ctx.strokeStyle = ACENTO; ctx.lineWidth = 2.5; ctx.strokeRect(x0 + a0[0] / 170 * (x1 - x0), ys[0] - 31, (a0[1] - a0[0] + 1) / 170 * (x1 - x0), 62); }
      // nodos
      for (l = 1; l < 7; l++) {
        var rad = n[l] === 1 ? 14 : Math.min(7, (x1 - x0) / n[l] / 2.6);
        for (var i = 0; i < n[l]; i++) {
          var dentro = r[l] && i >= r[l][0] && i <= r[l][1], es = l === sel.l && i === sel.i;
          ctx.beginPath(); ctx.arc(xNodo(l, i), ys[l], es ? rad + 3 : rad, 0, 7);
          ctx.fillStyle = es ? ACENTO : dentro ? NAVY : "#cfd6e2"; ctx.fill();
          if (l === 6) { ctx.lineWidth = 2; ctx.strokeStyle = NAVY; ctx.stroke(); }
        }
      }
      ctx.textAlign = "right"; ctx.font = "600 19px " + FUENTE;
      for (l = 0; l < 7; l++) { ctx.fillStyle = l === sel.l ? ACENTO : TINTA; ctx.fillText(capas[l].nom, x0 - 18, ys[l] + 2); ctx.font = "15px " + FUENTE; ctx.fillStyle = MUTED; ctx.fillText(l === 0 ? "170 col." : l === 6 ? "p" : n[l] + " pasos", x0 - 18, ys[l] + 20); ctx.font = "italic 600 15px " + FUENTE; ctx.fillStyle = capas[l].tipo === "MixConv" ? ACENTO : NAVY; if (capas[l].tipo) ctx.fillText(capas[l].tipo, x0 - 18, ys[l] + 38); ctx.font = "600 19px " + FUENTE; }
      var cols = r[0] ? r[0][1] - r[0][0] + 1 : 0, c = capas[sel.l];
      if ($("r-info")) $("r-info").innerHTML ="<h3>" + c.nom + (sel.l > 0 && sel.l < 6 ? " · paso " + (sel.i + 1) + " de " + n[sel.l] : "") + "</h3><p>" + c.info + "</p>" +
        (c.pesos ? "<p><b>" + c.pesos.toLocaleString("es-AR") + "</b> pesos</p>" : sel.l === 6 ? "<p>~24.000 pesos en las convoluciones</p>" : "") +
        (sel.l > 0 ? "<p>Mira <b>" + cols + " columnas</b> de entrada = " + coma(((cols - 1) * 10 + 30) / 1000, 2) + " s de audio</p>" : "");
    }
    cv.addEventListener("mousemove", function (e) {
      var q = posLocal(cv, e), mejor = null;
      for (var l = 1; l < 7; l++) if (Math.abs(q[1] - ys[l]) < 30) mejor = l;
      if (mejor === null || q[0] < x0) return;
      var i = n[mejor] === 1 ? 0 : Math.max(0, Math.min(n[mejor] - 1, Math.floor((q[0] - x0) / (x1 - x0) * n[mejor])));
      if (sel.l !== mejor || sel.i !== i) { sel = { l: mejor, i: i }; dibujar(); }
    });
    dibujar();
  }

  /* ───────── gráfico xy genérico ───────── */
  function grafico(cv, op) {
    var ctx = lienzo(cv), W = cv._w, Hh = cv._h, m = op.margen || [70, 20, 40, 44];
    var X = function (v) { return m[0] + (v - op.x[0]) / (op.x[1] - op.x[0]) * (W - m[0] - m[1]); };
    var Y = op.log ? function (v) { return m[2] + (1 - (Math.log10(Math.max(v, op.y[0])) - Math.log10(op.y[0])) / (Math.log10(op.y[1]) - Math.log10(op.y[0]))) * (Hh - m[2] - m[3]); }
                   : function (v) { return m[2] + (1 - (v - op.y[0]) / (op.y[1] - op.y[0])) * (Hh - m[2] - m[3]); };
    ctx.font = "16px " + FUENTE; ctx.fillStyle = MUTED; ctx.strokeStyle = "#c4c4c4"; ctx.lineWidth = 1;
    (op.yt || []).forEach(function (t) { ctx.beginPath(); ctx.moveTo(m[0], Y(t)); ctx.lineTo(W - m[1], Y(t)); ctx.stroke(); ctx.textAlign = "right"; ctx.fillText(op.yfmt ? op.yfmt(t) : t, m[0] - 8, Y(t) + 5); });
    (op.xt || []).forEach(function (t) { ctx.textAlign = "center"; ctx.fillText(op.xfmt ? op.xfmt(t) : t, X(t), Hh - m[3] + 22); });
    ctx.strokeStyle = "#444"; ctx.beginPath(); ctx.moveTo(m[0], m[2]); ctx.lineTo(m[0], Hh - m[3]); ctx.lineTo(W - m[1], Hh - m[3]); ctx.stroke();
    if (op.titulo) { ctx.textAlign = "left"; ctx.fillStyle = TINTA; ctx.font = "600 19px " + FUENTE; ctx.fillText(op.titulo, m[0] + 10, m[2] - 14); }
    return { ctx: ctx, X: X, Y: Y, W: W, H: Hh, m: m };
  }
  function linea(g, xs, ys, color, w, dash) {
    var ctx = g.ctx; ctx.strokeStyle = color; ctx.lineWidth = w || 2; ctx.setLineDash(dash || []); ctx.beginPath();
    xs.forEach(function (x, i) { var a = g.X(x), b = g.Y(ys[i]); i ? ctx.lineTo(a, b) : ctx.moveTo(a, b); }); ctx.stroke(); ctx.setLineDash([]);
  }
  var miles = function (v) { return v >= 1000 ? (v / 1000) + ".000" : v; };

  /* ───────── 23. entrenamiento ───────── */
  function iniEntreno() {
    var T = window.DATOS.entrenamiento, xs = T.perdida.map(function (_, i) { return T.desde + i * T.cada; });
    var vp = T.val.map(function (v) { return v[0]; });
    var g = grafico($("t-perd"), { x: [0, 45000], y: [0.002, 0.3], log: true, yt: [0.003, 0.01, 0.03, 0.1, 0.3], yfmt: function (t) { return coma(t, t < 0.01 ? 3 : t < 0.1 ? 2 : 1); },
      xt: [0, 10000, 20000, 30000, 40000], xfmt: miles, titulo: "Pérdida (escala log)" });
    linea(g, xs, T.perdida, NAVY, 2.5); linea(g, vp, T.val.map(function (v) { return v[3]; }), ACENTO, 2.5);
    var c = g.ctx; c.font = "16px " + FUENTE; c.textAlign = "right"; c.fillStyle = NAVY; c.fillText("entrenamiento (media móvil de 250 lotes)", g.W - 30, 50); c.fillStyle = ACENTO; c.fillText("validación (cada 500 pasos)", g.W - 30, 72);
    var g2 = grafico($("t-fah"), { x: [0, 45000], y: [0.3, 60], log: true, yt: [0.4, 1, 3, 10, 30], yfmt: function (t) { return coma(t, t < 1 ? 1 : 0); },
      xt: [0, 10000, 20000, 30000, 40000], xfmt: miles, titulo: "Falsas activaciones por hora en validación (umbral 0,5)" });
    linea(g2, [0, 45000], [0.4, 0.4], GRIS, 1.5, [6, 6]);
    linea(g2, vp, T.val.map(function (v) { return v[2]; }), NAVY2, 2.2);
    var ctx = g2.ctx;
    T.val.forEach(function (v) { ctx.beginPath(); ctx.arc(g2.X(v[0]), g2.Y(v[2]), 3, 0, 7); ctx.fillStyle = NAVY2; ctx.fill(); });
    var e = T.val.filter(function (v) { return v[0] === 11000; })[0];
    if (e) { ctx.beginPath(); ctx.arc(g2.X(e[0]), g2.Y(e[2]), 10, 0, 7); ctx.lineWidth = 3; ctx.strokeStyle = ACENTO; ctx.stroke();
      ctx.font = "600 17px " + FUENTE; ctx.fillStyle = ACENTO; ctx.textAlign = "left"; ctx.fillText("paso 11.000: " + coma(e[2], 2) + " por hora (mínimo)", g2.X(e[0]) + 18, g2.Y(e[2]) + 2); }
    ctx.font = "15px " + FUENTE; ctx.fillStyle = MUTED; ctx.textAlign = "right"; ctx.fillText("objetivo 0,4", g2.W - 24, g2.Y(0.4) - 6);
  }

  /* ───────── 25. detección ───────── */
  function iniDeteccion() {
    var D = window.DATOS.deteccion, feat = b64(D.feat, Uint8Array), probs = D.probs, dur = D.dur;
    var maxF = 0; for (var i = 0; i < feat.length; i++) maxF = Math.max(maxF, feat[i]);
    var img = imagenFeat(feat, D.cols, 0, D.cols, maxF);
    function dibujar() {
      var u = +$("d-u").value, Nv = +$("d-n").value, cut = u * 255;
      $("d-u-o").textContent = coma(u, 2); $("d-n-o").textContent = Nv + " (" + Nv * 30 + " ms)";
      var cv = $("d-graf"), ctx = lienzo(cv), W = cv._w, m0 = 70, m1 = 20, xT = function (t) { return m0 + t / dur * (W - m0 - m1); };
      var yS = 30, hS = 140, y0 = 210, hP = 262, yP = function (p) { return y0 + (1 - p) * hP; };
      ctx.imageSmoothingEnabled = false; ctx.drawImage(img, m0, yS, W - m0 - m1, hS);
      ctx.font = "600 18px " + FUENTE; ctx.textAlign = "left";
      [["charla en español", 0.1, TINTA], ["palabra parecida", D.marcas.parecida, ACENTO], ["charla", D.marcas.parecida + 1.1, TINTA], ["“Órbita”", D.marcas.orbita, NAVY], ["charla", D.marcas.orbita + 1.0, TINTA]].forEach(function (s) {
        ctx.fillStyle = s[2]; ctx.fillText(s[0], xT(s[1]) + 4, yS - 8);
        ctx.strokeStyle = s[2]; ctx.lineWidth = 2; ctx.beginPath(); ctx.moveTo(xT(s[1]), yS - 4); ctx.lineTo(xT(s[1]), yS + hS); ctx.stroke(); });
      // ejes de probabilidad
      ctx.font = "16px " + FUENTE; ctx.fillStyle = MUTED; ctx.strokeStyle = "#c4c4c4"; ctx.lineWidth = 1; ctx.textAlign = "right";
      [0, 0.25, 0.5, 0.75, 1].forEach(function (p) { ctx.beginPath(); ctx.moveTo(m0, yP(p)); ctx.lineTo(W - m1, yP(p)); ctx.stroke(); ctx.fillText(coma(p, 2), m0 - 8, yP(p) + 5); });
      ctx.textAlign = "center"; for (var t = 0; t <= dur; t += 1) ctx.fillText(t + " s", xT(t), y0 + hP + 26);
      // probabilidad cruda y promedio
      var tk = function (k) { return (k + 1) * 0.03; }, prom = [], det = [], ult = [], ign = -100, maxCharla = 0;
      probs.forEach(function (q, k) {
        ult.push(q); if (ult.length > Nv) ult.shift();
        var pm = ult.reduce(function (a, b) { return a + b; }, 0) / ult.length; prom.push(pm);
        if (q < cut) ign = Math.min(ign + 1, 0);
        if (ign === 0 && ult.length === Nv && pm > cut) { det.push(tk(k)); ult = []; ign = -100; }
        if (tk(k) < D.marcas.orbita) maxCharla = Math.max(maxCharla, pm);
      });
      ctx.strokeStyle = ACERO; ctx.lineWidth = 1.5; ctx.beginPath(); probs.forEach(function (q, k) { var a = xT(tk(k)), b = yP(q / 255); k ? ctx.lineTo(a, b) : ctx.moveTo(a, b); }); ctx.stroke();
      ctx.strokeStyle = NAVY; ctx.lineWidth = 3.5; ctx.beginPath(); prom.forEach(function (q, k) { var a = xT(tk(k)), b = yP(q / 255); k ? ctx.lineTo(a, b) : ctx.moveTo(a, b); }); ctx.stroke();
      ctx.strokeStyle = ACENTO; ctx.lineWidth = 2.5; ctx.setLineDash([10, 7]); ctx.beginPath(); ctx.moveTo(m0, yP(u)); ctx.lineTo(W - m1, yP(u)); ctx.stroke(); ctx.setLineDash([]);
      ctx.fillStyle = "rgba(93,97,104,.10)"; ctx.fillRect(m0, y0, xT(3) - m0, hP);
      ctx.fillStyle = MUTED; ctx.textAlign = "left"; ctx.font = "15px " + FUENTE; ctx.fillText("espera inicial (100 inferencias)", m0 + 8, y0 + hP - 10);
      det.forEach(function (t) { ctx.fillStyle = "rgba(28,47,87,.14)"; ctx.fillRect(xT(t) - 3, yS, 6, y0 + hP - yS);
        ctx.fillStyle = NAVY; ctx.font = "600 19px " + FUENTE; ctx.textAlign = "right"; ctx.fillText("DETECCIÓN · " + coma(t, 2) + " s", xT(t) - 12, y0 + 26); });
      ctx.font = "16px " + FUENTE; ctx.textAlign = "right"; ctx.fillStyle = ACERO; ctx.fillText("p cruda (cada 30 ms)", W - 30, y0 + 50); ctx.fillStyle = NAVY; ctx.fillText("promedio de " + Nv, W - 30, y0 + 72);
      $("d-lect").innerHTML = "<dt>Detecciones</dt><dd>" + det.length + "</dd><dt>Máximo antes de “Órbita”</dt><dd>" + coma(maxCharla / 255, 2) + "</dd>";
    }
    ["d-u", "d-n"].forEach(function (id) { $(id).addEventListener("input", dibujar); });
    dibujar();
  }

  /* ───────── dispositivo 3D (diapo 2): proyección ortográfica, se gira arrastrando ───────── */
  function iniDispositivo() { [].forEach.call(document.querySelectorAll("canvas.disp3d"), dispositivo3d); }
  function dispositivo3d(cv) {
    var yaw = 0.55, pitch = 0.42, S = 128, OX = 270, OY = 200, ZP = 1.1;
    var LUZ = [-0.45, -0.55, 0.7], ln = Math.hypot(LUZ[0], LUZ[1], LUZ[2]); LUZ = LUZ.map(function (v) { return v / ln; });
    function mezcla(a, b, t) {
      var p = function (h, i) { return parseInt(h.slice(1 + 2 * i, 3 + 2 * i), 16); };
      return "rgb(" + [0, 1, 2].map(function (i) { return Math.round(p(a, i) + (p(b, i) - p(a, i)) * t); }).join(",") + ")";
    }
    function rot(v) { var c = Math.cos(yaw), s = Math.sin(yaw); return [v[0] * c - v[1] * s, v[0] * s + v[1] * c, v[2]]; }
    function P(v) {
      var r = rot(v), cp = Math.cos(pitch), sp = Math.sin(pitch);
      return [OX + S * r[0], OY - S * ((r[2] - ZP) * cp + r[1] * sp), r[1] * cp - r[2] * sp];
    }
    function visible(n) { var r = rot(n); return -r[1] * Math.cos(pitch) + r[2] * Math.sin(pitch) > 0; }
    function luz(n) { var r = rot(n); return Math.max(0, r[0] * LUZ[0] + r[1] * LUZ[1] + r[2] * LUZ[2]); }
    function rr(a, b, r, z, n) {          // rectángulo redondeado, recorrido antihorario visto desde arriba
      var out = [], cs = [[a - r, b - r, 0], [-(a - r), b - r, 1], [-(a - r), -(b - r), 2], [a - r, -(b - r), 3]];
      cs.forEach(function (c) { for (var k = 0; k <= n; k++) { var t = (c[2] + k / n) * Math.PI / 2; out.push([c[0] + r * Math.cos(t), c[1] + r * Math.sin(t), z]); } });
      return out;
    }
    function circ(cx, cy, z, r) { var o = []; for (var k = 0; k < 14; k++) { var t = k / 14 * 2 * Math.PI; o.push([cx + r * Math.cos(t), cy + r * Math.sin(t), z]); } return o; }
    function poli(ctx, pts) { ctx.beginPath(); pts.forEach(function (p, i) { var q = P(p); i ? ctx.lineTo(q[0], q[1]) : ctx.moveTo(q[0], q[1]); }); ctx.closePath(); }
    function linea(ctx, a, b) { var p = P(a), q = P(b); ctx.moveTo(p[0], p[1]); ctx.lineTo(q[0], q[1]); }

    /* base: tres franjas de pared (la del medio es la luz azul) + chaflán superior */
    var A = 1.15, B = 0.72, RC = 0.38, N = 8, ZT = 0.5;
    var nivel = [0, 0.06, 0.12, 0.42];
    var lazos = nivel.map(function (z) { return rr(A, B, RC, z, N); }).concat([rr(A - 0.07, B - 0.07, RC - 0.07, ZT, N)]);
    var M = lazos[0].length;
    var normPared = lazos[0].map(function (p, i) { var q = lazos[0][(i + 1) % M], dx = q[0] - p[0], dy = q[1] - p[1], l = Math.hypot(dx, dy) || 1; return [dy / l, -dx / l, 0]; });
    var normChaf = normPared.map(function (n) { var l = Math.hypot(n[0], n[1], 0.9); return [n[0] / l, n[1] / l, 0.9 / l]; });

    /* aro (toro vertical en el plano x-z), soporte y esfera */
    var R = 0.82, T = 0.09, ZC = 0.6 + R, NU = 72, NV = 16;
    function toro(u, v) { var e = [Math.cos(u), 0, Math.sin(u)]; return [R * e[0] + T * Math.cos(v) * e[0], T * Math.sin(v), ZC + R * e[2] + T * Math.cos(v) * e[2]]; }
    var caras3 = [];
    for (var i = 0; i < NU; i++) for (var j = 0; j < NV; j++) {
      var u0 = i / NU * 2 * Math.PI, u1 = (i + 1) / NU * 2 * Math.PI, v0 = j / NV * 2 * Math.PI, v1 = (j + 1) / NV * 2 * Math.PI;
      var um = (u0 + u1) / 2, vm = (v0 + v1) / 2;
      caras3.push({ tipo: "aro", pts: [toro(u0, v0), toro(u1, v0), toro(u1, v1), toro(u0, v1)], n: [Math.cos(vm) * Math.cos(um), Math.sin(vm), Math.cos(vm) * Math.sin(um)] });
    }
    var sx = 0.17, sy = 0.12, z0 = ZT, z1 = 0.66;
    var cub = function (x, y, z) { return [x * sx, y * sy, z ? z1 : z0]; };
    [[[1, -1, 0], [1, 1, 0], [1, 1, 1], [1, -1, 1], [1, 0, 0]], [[-1, 1, 0], [-1, -1, 0], [-1, -1, 1], [-1, 1, 1], [-1, 0, 0]],
     [[-1, -1, 0], [1, -1, 0], [1, -1, 1], [-1, -1, 1], [0, -1, 0]], [[1, 1, 0], [-1, 1, 0], [-1, 1, 1], [1, 1, 1], [0, 1, 0]],
     [[-1, -1, 1], [1, -1, 1], [1, 1, 1], [-1, 1, 1], [0, 0, 1]]].forEach(function (f) {
      caras3.push({ tipo: "soporte", pts: f.slice(0, 4).map(function (c) { return cub(c[0], c[1], c[2]); }), n: f[4] });
    });
    var ua = 0.72, esf = { tipo: "esfera", c: [R * Math.cos(ua) + 0.04, -0.13, ZC + R * Math.sin(ua)], r: 0.25 };

    function dentro(q) {        // caras del aro tapadas por la esfera: no se dibujan
      var c = [0, 1, 2].map(function (e) { return (q[0][e] + q[1][e] + q[2][e] + q[3][e]) / 4; });
      return Math.hypot(c[0] - esf.c[0], c[1] - esf.c[1], c[2] - esf.c[2]) < esf.r + 0.05;
    }

    function dibujar() {
      var ctx = lienzo(cv);
      ctx.lineJoin = "round"; ctx.lineCap = "round";
      /* sombra sobre la mesa */
      ctx.save(); ctx.shadowColor = "rgba(28,47,87,0.35)"; ctx.shadowBlur = 30; ctx.fillStyle = "rgba(28,47,87,0.10)";
      poli(ctx, rr(A + 0.08, B + 0.08, RC + 0.08, 0, N)); ctx.fill(); ctx.restore();

      /* base (convexa y por debajo de todo: alcanza con descartar caras traseras) */
      for (var k = 0; k < 4; k++) for (var i = 0; i < M; i++) {
        var i2 = (i + 1) % M, n = k < 3 ? normPared[i] : normChaf[i];
        if (!visible(n)) continue;
        var q = [lazos[k][i], lazos[k][i2], lazos[k + 1][i2], lazos[k + 1][i]], t = luz(n);
        ctx.fillStyle = k === 1 ? mezcla("#2b63d6", "#8fbaff", t) : mezcla("#b7c4da", "#f6f8fb", t);
        ctx.strokeStyle = ctx.fillStyle; ctx.lineWidth = 0.8; poli(ctx, q); ctx.fill(); ctx.stroke();
        ctx.beginPath(); ctx.strokeStyle = NAVY; ctx.lineWidth = 2.2;
        if (k === 0) linea(ctx, q[0], q[1]);
        if (k === 3) linea(ctx, q[2], q[3]);
        var nPrev = k < 3 ? normPared[(i - 1 + M) % M] : normChaf[(i - 1 + M) % M], nSig = k < 3 ? normPared[i2] : normChaf[i2];
        if (!visible(nPrev)) linea(ctx, q[0], q[3]);
        if (!visible(nSig)) linea(ctx, q[1], q[2]);
        ctx.stroke();
        if (k === 2) { ctx.beginPath(); ctx.lineWidth = 1; ctx.strokeStyle = "rgba(28,47,87,0.35)"; linea(ctx, q[2], q[3]); ctx.stroke(); }
      }
      ctx.fillStyle = "#ffffff"; poli(ctx, lazos[4]); ctx.fill();
      /* detalles de la tapa: fila de agujeros y botón central */
      ctx.fillStyle = "rgba(28,47,87,0.55)";
      [-0.5, -0.38, -0.26, 0.26, 0.38, 0.5].forEach(function (x) { poli(ctx, circ(x, -0.42, ZT, 0.022)); ctx.fill(); });
      ctx.fillStyle = NAVY2; poli(ctx, circ(0, -0.42, ZT, 0.05)); ctx.fill();

      /* brillo del aro */
      ctx.save(); ctx.beginPath();
      for (var a = 0; a <= 64; a++) { var u = a / 64 * 2 * Math.PI, p = P([R * Math.cos(u), 0, ZC + R * Math.sin(u)]); a ? ctx.lineTo(p[0], p[1]) : ctx.moveTo(p[0], p[1]); }
      ctx.strokeStyle = "rgba(90,150,255,0.22)"; ctx.lineWidth = 34; ctx.shadowColor = "rgba(70,130,255,0.7)"; ctx.shadowBlur = 40; ctx.stroke(); ctx.restore();

      /* aro, soporte y esfera ordenados por profundidad (algoritmo del pintor) */
      var lista = caras3.filter(function (f) { return visible(f.n) && !(f.tipo === "aro" && dentro(f.pts)); }).map(function (f) {
        var pr = f.pts.map(P); f.pr = pr; f.d = (pr[0][2] + pr[1][2] + pr[2][2] + pr[3][2]) / 4; return f;
      });
      var pe = P(esf.c); esf.d = pe[2] - 0.05; lista.push(esf);
      lista.sort(function (x, y) { return y.d - x.d; });
      lista.forEach(function (f) {
        if (f.tipo === "esfera") {
          var r = esf.r * S, g = ctx.createRadialGradient(pe[0] - r * 0.35, pe[1] - r * 0.4, r * 0.1, pe[0], pe[1], r);
          g.addColorStop(0, "#5a6fa6"); g.addColorStop(1, NAVY);
          ctx.beginPath(); ctx.arc(pe[0], pe[1], r, 0, 2 * Math.PI); ctx.fillStyle = g; ctx.fill();
          ctx.lineWidth = 2.2; ctx.strokeStyle = NAVY; ctx.stroke(); return;
        }
        var t = luz(f.n);
        ctx.beginPath(); f.pr.forEach(function (p, i) { i ? ctx.lineTo(p[0], p[1]) : ctx.moveTo(p[0], p[1]); }); ctx.closePath();
        if (f.tipo === "aro") { ctx.fillStyle = mezcla("#2f6ae0", "#cfe2ff", Math.pow(t, 1.6)); ctx.strokeStyle = ctx.fillStyle; ctx.lineWidth = 0.9; ctx.fill(); ctx.stroke(); }
        else { ctx.fillStyle = mezcla("#c3cee0", "#ffffff", t); ctx.fill(); ctx.strokeStyle = NAVY; ctx.lineWidth = 2; ctx.stroke(); }
      });
    }

    /* interacción: arrastrar gira (horizontal = giro, vertical = inclinación); quieto, rota solo */
    var arr = null, ultimo = -1e9;
    cv.addEventListener("pointerdown", function (e) { arr = [e.clientX, e.clientY, yaw, pitch]; cv.setPointerCapture(e.pointerId); cv.style.cursor = "grabbing"; });
    cv.addEventListener("pointermove", function (e) {
      if (!arr) return;
      yaw = arr[2] + (e.clientX - arr[0]) * 0.012;
      pitch = Math.max(0.12, Math.min(0.72, arr[3] + (e.clientY - arr[1]) * 0.008));
      dibujar();
    });
    var soltar = function () { arr = null; ultimo = performance.now(); cv.style.cursor = "grab"; };
    cv.addEventListener("pointerup", soltar); cv.addEventListener("pointercancel", soltar);
    var sec = cv.closest("section"), t0 = performance.now();
    (function giro(t) {
      var dt = Math.min(0.05, (t - t0) / 1000); t0 = t;
      if (!arr && sec.classList.contains("activa") && t - ultimo > 2500) { yaw += dt * 0.35; dibujar(); }
      requestAnimationFrame(giro);
    })(t0);
    dibujar();
  }

  /* fórmulas que no entran en su columna: se achica la fuente de ese bloque */
  window.ajustarFormulas = function () {
    document.body.classList.add("medir");
    [].forEach.call(document.querySelectorAll(".eq"), function (eq) {
      if (!eq.querySelector("mjx-container")) return;
      var disp = eq.clientWidth, ancho = eq.scrollWidth;
      if (ancho > disp + 2 && disp > 0) {
        var fs = parseFloat(getComputedStyle(eq).fontSize);
        eq.style.fontSize = Math.max(20, Math.floor(fs * disp / ancho * 0.97)) + "px";
      }
    });
    document.body.classList.remove("medir");
  };

  /* arranque: todo dibujado de entrada (también sirve para el PDF) */
  function arrancar() {
    [iniDispositivo, iniComparacion, iniHiper, iniPrecio, iniMedicion, iniEspectro, iniEtapas, iniRed, iniEntreno, iniDeteccion].forEach(function (f) {
      try { f(); } catch (err) { console.error(f.name, err); }
    });
  }
  if (document.fonts && document.fonts.ready) document.fonts.ready.then(arrancar); else arrancar();
})();
