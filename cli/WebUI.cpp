// The local web UI.
//
// Frontend only. Everything here calls into sb53_core through the same interfaces the
// CLI uses; no algorithm logic lives in this file (ADR-0002).

#include "WebUI.hpp"

#include "HttpServer.hpp"

#include "sb53/FlowAnalysis.hpp"
#include "sb53/GcodeRewriter.hpp"
#include "sb53/GcodeScanner.hpp"
#include "sb53/SubprocessRunner.hpp"
#include "sb53/TemperaturePlanner.hpp"
#include "sb53/Version.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#pragma comment(lib, "Comdlg32.lib")
#endif

namespace sb53::web {
namespace {

// Opens a native file dialog and returns the chosen path.
//
// A browser deliberately cannot hand a server a local file path -- that would be a
// serious information leak on the open web. But this server IS the user's machine, so it
// can ask the operating system directly. The page calls /api/browse and gets a real path
// back, which beats copying and pasting one.
std::string chooseGcodeFile()
{
#ifdef _WIN32
    wchar_t path[MAX_PATH * 4]{};

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"G-code files\0*.gcode;*.gco;*.g\0All files\0*.*\0\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = static_cast<DWORD>(std::size(path));
    ofn.lpstrTitle = L"Select a G-code file";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;

    if (!::GetOpenFileNameW(&ofn)) {
        return {};   // cancelled
    }

    const int size = ::WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0,
                                           nullptr, nullptr);
    if (size <= 1) {
        return {};
    }
    std::string result(static_cast<std::size_t>(size - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, path, -1, result.data(), size, nullptr, nullptr);
    return result;
#else
    return {};
#endif
}

// Distinct scratch-directory names. Uses a counter as well as the clock, because two
// requests can land inside the same clock tick.
[[nodiscard]] unsigned long long uniqueSuffix()
{
    static std::atomic<unsigned long long> counter{0};
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return static_cast<unsigned long long>(now) * 1000ull + counter.fetch_add(1);
}

constexpr std::string_view kPage = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Flow&amp;Temp (C++) &mdash; G-Code Controller</title>
<style>
:root{color-scheme:light dark;--bg:#fbfbfc;--panel:#fff;--ink:#1a1c20;--muted:#6b7280;
--line:#e3e5e9;--accent:#2563eb;--warn:#b45309;--err:#b91c1c;--ok:#15803d;
--flow:#2563eb;--temp:#dc2626;--drop:#7c3aed}
@media(prefers-color-scheme:dark){:root{--bg:#15171a;--panel:#1c1f24;--ink:#e8eaed;
--muted:#9aa1ab;--line:#2c3037;--accent:#60a5fa;--warn:#fbbf24;--err:#f87171;--ok:#4ade80;
--flow:#60a5fa;--temp:#f87171;--drop:#c4b5fd}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--ink);
font:14px/1.5 ui-sans-serif,system-ui,-apple-system,"Segoe UI",sans-serif}
header{padding:14px 20px;border-bottom:1px solid var(--line);display:flex;
align-items:baseline;gap:12px;flex-wrap:wrap}
h1{font-size:16px;margin:0;font-weight:600}
.ver{color:var(--muted);font-size:12px}
.warn{margin-left:auto;color:var(--warn);font-size:12px}
main{display:grid;grid-template-columns:340px 1fr;gap:16px;padding:16px;align-items:start}
@media(max-width:900px){main{grid-template-columns:1fr}}
.panel{background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:14px}
fieldset{border:0;padding:0;margin:0 0 16px}
legend{font-weight:600;font-size:12px;text-transform:uppercase;letter-spacing:.04em;
color:var(--muted);padding:0 0 8px}
label{display:block;margin-bottom:9px}
label span{display:block;font-size:12px;color:var(--muted);margin-bottom:3px}
input{width:100%;padding:7px 9px;border:1px solid var(--line);border-radius:6px;
background:var(--bg);color:var(--ink);font:inherit;font-variant-numeric:tabular-nums}
input:focus{outline:2px solid var(--accent);outline-offset:-1px}
.row{display:grid;grid-template-columns:1fr 1fr 1fr;gap:7px}
.row2{display:grid;grid-template-columns:1fr 1fr;gap:7px}
button{padding:9px 14px;border-radius:7px;border:1px solid var(--accent);
background:var(--accent);color:#fff;font:inherit;font-weight:600;cursor:pointer}
button.secondary{background:transparent;color:var(--accent)}
button:disabled{opacity:.5;cursor:progress}
.actions{display:flex;gap:8px}
canvas{width:100%;height:340px;display:block}
table{border-collapse:collapse;width:100%;font-variant-numeric:tabular-nums}
td{padding:3px 0;border-bottom:1px solid var(--line)}
td:last-child{text-align:right;font-weight:600}
.msg{padding:10px 12px;border-radius:7px;margin-bottom:12px;font-size:13px;
white-space:pre-wrap}
.msg.err{background:color-mix(in srgb,var(--err) 12%,transparent);color:var(--err)}
.msg.ok{background:color-mix(in srgb,var(--ok) 12%,transparent);color:var(--ok)}
.msg.warn{background:color-mix(in srgb,var(--warn) 14%,transparent);color:var(--warn)}
.legend{display:flex;gap:16px;flex-wrap:wrap;font-size:12px;color:var(--muted);
margin-bottom:8px}
.key{display:inline-block;width:22px;height:0;border-top-width:3px;vertical-align:middle;
margin-right:5px}
.hint{font-size:12px;color:var(--muted);margin:-4px 0 10px}
.pick{display:flex;gap:6px}
.pick input{flex:1;min-width:0}
.pick button{white-space:nowrap;padding:7px 12px;font-weight:500}
.empty{color:var(--muted);text-align:center;padding:60px 20px}
</style>
</head>
<body>
<header>
  <h1>G-Code Flow &amp; Temperature Controller</h1>
  <span class="ver" id="ver"></span>
  <span class="ver">C++ Edition &mdash; not the original SB53-Systems app</span>
  <span class="warn">Not yet validated by test prints &mdash; check output before printing</span>
</header>
<main>
  <form class="panel" id="form">
    <fieldset>
      <legend>Input</legend>
      <label><span>G-code file</span>
        <div class="pick">
          <input name="path" id="path" placeholder="choose a file, or paste a path" required>
          <button type="button" id="browse" class="secondary">Browse&hellip;</button>
        </div></label>
      <label><span>klipper_estimator.exe (blank = auto)</span>
        <input name="estimator" id="estimator" placeholder="auto-detected"></label>
    </fieldset>

    <fieldset>
      <legend>Filament &mdash; flow (mm&sup3;/s)</legend>
      <div class="row">
        <label><span>low</span><input name="low" value="1" inputmode="decimal"></label>
        <label><span>mid</span><input name="mid" value="80" inputmode="decimal"></label>
        <label><span>high</span><input name="high" value="105" inputmode="decimal"></label>
      </div>
      <legend>Filament &mdash; temperature (&deg;C)</legend>
      <div class="row">
        <label><span>low</span><input name="lowTemp" value="220" inputmode="decimal"></label>
        <label><span>mid</span><input name="midTemp" value="280" inputmode="decimal"></label>
        <label><span>high</span><input name="highTemp" value="310" inputmode="decimal"></label>
      </div>
      <p class="hint">Flow points must strictly increase.</p>
    </fieldset>

    <fieldset>
      <legend>Hotend</legend>
      <div class="row2">
        <label><span>heats &deg;C/s</span><input name="rise" value="5" inputmode="decimal"></label>
        <label><span>cools &deg;C/s</span><input name="fall" value="1" inputmode="decimal"></label>
      </div>
      <div class="row2">
        <label><span>smoothing (s)</span><input name="smoothing" value="20" inputmode="numeric"></label>
        <label><span>bias 0&ndash;10</span><input name="bias" value="7" inputmode="numeric"></label>
      </div>
      <p class="hint">Bias 0 = quality (smooth), 10 = most aggressive.
      Migrating from the old tool? Use <b>10 &minus; its value</b>.</p>
    </fieldset>

    <fieldset>
      <legend>Fast-layer cooling</legend>
      <div class="row2">
        <label><span>layers under (s)</span><input name="coolBelow" value="5" inputmode="decimal"></label>
        <label><span>drop up to (&deg;C)</span><input name="coolDrop" value="0" inputmode="decimal"></label>
      </div>
      <p class="hint">0 disables. Analyse first &mdash; the layer-time chart shows
      whether your threshold catches only the fast layers or the whole print.</p>
    </fieldset>

    <fieldset>
      <legend>Output</legend>
      <label><span>Write to (blank = overwrite input)</span>
        <input name="out" id="out" placeholder="leave blank to overwrite"></label>
    </fieldset>

    <div class="actions">
      <button type="submit" id="analyze">Analyse</button>
      <button type="button" class="secondary" id="process">Process &amp; write</button>
    </div>
  </form>

  <div>
    <div class="panel" style="margin-bottom:16px">
      <div id="messages"></div>
      <div class="legend">
        <span><i class="key" style="border-top:3px solid var(--flow)"></i>flow mm&sup3;/s</span>
        <span><i class="key" style="border-top:3px solid var(--temp)"></i>temperature &deg;C</span>
        <span><i class="key" style="border-top:3px dashed var(--drop)"></i>layer-time cooling</span>
      </div>
      <canvas id="chart" width="1200" height="680"></canvas>
      <div class="empty" id="empty">Enter a file path and press Analyse.</div>
    </div>
    <div class="panel">
      <table id="stats"></table>
    </div>
  </div>
</main>
<script>
const $ = s => document.querySelector(s);
let data = null;

fetch('/api/version').then(r=>r.json()).then(v=>{$('#ver').textContent = 'v'+v.version;});

function msg(kind, text){
  $('#messages').innerHTML = '<div class="msg '+kind+'">'+
    text.replace(/&/g,'&amp;').replace(/</g,'&lt;')+'</div>';
}
function clearMsg(){ $('#messages').innerHTML=''; }

function body(){ return new URLSearchParams(new FormData($('#form'))).toString(); }

async function call(endpoint, btn){
  clearMsg();
  btn.disabled = true;
  const old = btn.textContent; btn.textContent = 'Working...';
  try{
    const r = await fetch(endpoint, {method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded'}, body: body()});
    const j = await r.json();
    if(j.error){ msg('err', j.error); return null; }
    return j;
  }catch(e){ msg('err', 'Request failed: '+e); return null; }
  finally{ btn.disabled=false; btn.textContent=old; }
}

$('#browse').addEventListener('click', async ()=>{
  const btn = $('#browse');
  btn.disabled = true; const old = btn.textContent; btn.textContent = 'Choosing...';
  try{
    // The dialog opens on the machine running the server, which is this machine.
    const r = await fetch('/api/browse', {method:'POST'});
    const j = await r.json();
    if(j.path){ $('#path').value = j.path; clearMsg(); }
  }catch(e){ msg('err','Could not open the file dialog: '+e); }
  finally{ btn.disabled=false; btn.textContent=old; }
});

$('#form').addEventListener('submit', async e=>{
  e.preventDefault();
  const j = await call('/api/analyze', $('#analyze'));
  if(!j) return;
  data = j;
  $('#empty').style.display='none';
  if(j.warning) msg('warn', j.warning);
  draw(); stats(j);
});

$('#process').addEventListener('click', async ()=>{
  if(!confirm('Write the processed G-code?\n\nThis tool has not been validated by test '
    +'prints. Check the output before sending it to a printer.')) return;
  const j = await call('/api/process', $('#process'));
  if(!j) return;
  msg('ok', 'Wrote '+j.output+'\n'+j.temperatureCommands+' temperature commands, '
    +j.feedratesReduced+' feedrates reduced.');
});

function stats(j){
  const rows = [
    ['moves', j.moves], ['estimated time', fmtTime(j.time)],
    ['filament', j.filament.toFixed(1)+' mm'],
    ['peak flow', j.peakFlow.toFixed(1)+' mm\u00b3/s'],
    ['mean flow', j.meanFlow.toFixed(1)+' mm\u00b3/s'],
    ['layers', j.layerCount],
    ['median layer time', j.medianLayer.toFixed(2)+' s'],
    ['start temperature', j.initialTemp.toFixed(1)+' \u00b0C'],
    ['planned range', j.minTemp.toFixed(1)+' \u2013 '+j.maxTemp.toFixed(1)+' \u00b0C'],
  ];
  $('#stats').innerHTML = rows.map(r=>'<tr><td>'+r[0]+'</td><td>'+r[1]+'</td></tr>').join('');
}
function fmtTime(s){ const m=Math.floor(s/60); return m+'m '+Math.round(s-m*60)+'s'; }

function draw(){
  const c = $('#chart'), g = c.getContext('2d');
  const cs = getComputedStyle(document.documentElement);
  const W = c.width, H = c.height;
  g.clearRect(0,0,W,H);
  if(!data || !data.seconds.length) return;

  const S = data.seconds;
  const padL=62, padR=62, padT=18, padB=26;
  const mainH = Math.round(H*0.62), gap=34;
  const layerTop = padT+mainH+gap, layerH = H-layerTop-padB;
  const plotW = W-padL-padR;

  const maxFlow = Math.max(...S.map(p=>p.flow), 1);
  const tLo = Math.min(...S.map(p=>p.temp)) - 3;
  const tHi = Math.max(...S.map(p=>p.temp)) + 3;
  const tEnd = S[S.length-1].t;

  const x = t => padL + (t/tEnd)*plotW;
  const yF = v => padT + mainH - (v/maxFlow)*mainH;
  const yT = v => padT + mainH - ((v-tLo)/(tHi-tLo||1))*mainH;

  const line = cs.getPropertyValue('--line').trim();
  const muted = cs.getPropertyValue('--muted').trim();

  // axes
  g.strokeStyle=line; g.lineWidth=1; g.font='11px ui-sans-serif,sans-serif';
  for(let i=0;i<=4;i++){
    const y = padT + (mainH/4)*i;
    g.beginPath(); g.moveTo(padL,y); g.lineTo(W-padR,y); g.stroke();
    g.fillStyle=muted; g.textAlign='right';
    g.fillText((maxFlow*(1-i/4)).toFixed(0), padL-7, y+4);
    g.textAlign='left';
    g.fillText((tLo+(tHi-tLo)*(1-i/4)).toFixed(0)+'\u00b0', W-padR+7, y+4);
  }
  g.textAlign='center'; g.fillStyle=muted;
  for(let i=0;i<=6;i++){
    const t=(tEnd/6)*i;
    g.fillText(fmtTime(t), x(t), padT+mainH+16);
  }

  const plot=(key, colour, proj, dash)=>{
    g.beginPath(); g.strokeStyle=colour; g.lineWidth=1.8; g.setLineDash(dash||[]);
    S.forEach((p,i)=>{ const X=x(p.t), Y=proj(p[key]); i?g.lineTo(X,Y):g.moveTo(X,Y); });
    g.stroke(); g.setLineDash([]);
  };
  plot('flow', cs.getPropertyValue('--flow').trim(), yF);
  plot('temp', cs.getPropertyValue('--temp').trim(), yT);
  if(S.some(p=>p.drop>0)){
    plot('drop', cs.getPropertyValue('--drop').trim(),
         v=> padT+mainH-(v/Math.max(...S.map(p=>p.drop),1))*mainH*0.28, [5,4]);
  }

  // layer-time strip: the evidence for choosing a cooling threshold
  const L = data.layers;
  if(L.length){
    const maxL = Math.max(...L.map(l=>l.duration), 0.001);
    const bw = plotW/L.length;
    const thr = data.coolBelow;
    L.forEach((l,i)=>{
      const h = Math.max(1,(l.duration/maxL)*layerH);
      g.fillStyle = (thr>0 && l.duration<thr)
        ? cs.getPropertyValue('--drop').trim() : line;
      g.fillRect(padL+i*bw, layerTop+layerH-h, Math.max(1,bw-0.5), h);
    });
    if(thr>0 && thr<maxL){
      const y = layerTop+layerH-(thr/maxL)*layerH;
      g.strokeStyle=cs.getPropertyValue('--warn').trim(); g.setLineDash([4,3]);
      g.beginPath(); g.moveTo(padL,y); g.lineTo(W-padR,y); g.stroke(); g.setLineDash([]);
    }
    g.fillStyle=muted; g.textAlign='left';
    g.fillText('layer time \u2014 '+L.length+' layers, median '
      +data.medianLayer.toFixed(2)+'s'
      +(thr>0? ', '+L.filter(l=>l.duration<thr).length+' below threshold':''),
      padL, layerTop-8);
  }
}
window.addEventListener('resize', ()=>{ if(data) draw(); });
</script>
</body>
</html>)HTML";

// Builds profiles from the submitted form.
struct FormSettings {
    ExtruderProfile extruder;
    FilamentProfile filament;
    std::filesystem::path estimator;
};

FormSettings readSettings(const Request& r)
{
    FormSettings s;
    s.filament.lowFlow  = r.number("low", 1.0);
    s.filament.midFlow  = r.number("mid", 80.0);
    s.filament.highFlow = r.number("high", 105.0);
    s.filament.lowTemp  = r.number("lowTemp", 220.0);
    s.filament.midTemp  = r.number("midTemp", 280.0);
    s.filament.highTemp = r.number("highTemp", 310.0);
    s.filament.speedQualityBias = static_cast<int>(r.number("bias", 7));

    s.extruder.tempRise = r.number("rise", 5.0);
    s.extruder.tempFall = r.number("fall", 1.0);
    s.extruder.smoothingWindow = static_cast<int>(r.number("smoothing", 20));
    s.extruder.coolingLayerTime = r.number("coolBelow", 5.0);
    s.extruder.coolingMaxDrop = r.number("coolDrop", 0.0);
    s.extruder.startMacro = "PRINT_START";
    s.extruder.temperatureToken = "EXTRUDER_TEMP";

    const auto est = r.field("estimator");
    if (!est.empty()) {
        s.estimator = est;
    }
    return s;
}

std::string firstError(const DiagnosticList& diags)
{
    for (const auto& d : diags) {
        if (d.isError()) {
            return std::string(toString(d.code)) + ": " + d.message;
        }
    }
    return {};
}

std::string firstWarning(const DiagnosticList& diags)
{
    for (const auto& d : diags) {
        if (d.severity == Severity::Warning) {
            return d.message;
        }
    }
    return {};
}

// One pipeline run up to the plan, shared by analyze and process.
struct RunResult {
    bool ok = false;
    std::string error;
    std::string warning;
    ScanResult scan;
    SourceAnalysis analysis;
    TemperaturePlan plan;
    FormSettings settings;
    std::filesystem::path estimator;
};

RunResult runToPlan(const Request& request, const std::filesystem::path& exeDir,
                    const std::function<std::filesystem::path(const std::filesystem::path&)>& findEstimator)
{
    RunResult out;
    out.settings = readSettings(request);

    const std::string path = request.field("path");
    if (path.empty()) {
        out.error = "No G-code file given.";
        return out;
    }
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        out.error = "File not found: " + path;
        return out;
    }

    out.estimator = out.settings.estimator.empty() ? findEstimator(exeDir)
                                                   : out.settings.estimator;
    if (out.estimator.empty()) {
        out.error = "Could not find klipper_estimator.exe. Give its full path.";
        return out;
    }
    const auto config = out.estimator.parent_path() / "config.json";
    if (!std::filesystem::exists(config, ec)) {
        out.error = "No config.json beside the estimator (" + config.string() + ").";
        return out;
    }

    DiagnosticList diags;
    {
        std::ifstream in{path, std::ios::binary};
        out.scan = GcodeScanner::scan(in, diags);
    }
    if (diags.hasErrors()) {
        out.error = firstError(diags);
        return out;
    }

    // Estimator sees the print body only -- see the note in the CLI's runProcess.
    const auto scratch = std::filesystem::temp_directory_path(ec) /
                         ("sb53-web-" + std::to_string(uniqueSuffix()));
    std::filesystem::create_directories(scratch, ec);
    const auto bodyPath = scratch / "body.gcode";
    {
        std::ifstream in{path, std::ios::binary};
        std::ofstream body{bodyPath, std::ios::binary};
        std::string line;
        std::size_t n = 0;
        while (std::getline(in, line)) {
            ++n;
            if (n >= out.scan.bodyFirstLine && n <= out.scan.bodyLastLine) {
                if (!line.empty() && line.back() == '\r') { line.pop_back(); }
                body << line << '\n';
            }
        }
    }

    SubprocessRunner runner;
    const auto proc = runner.run(
        out.estimator,
        {"--config_file", config.string(), "dump-moves", bodyPath.string()},
        std::chrono::minutes{10});
    std::filesystem::remove_all(scratch, ec);

    if (proc.launchFailed || proc.timedOut || proc.exitCode != 0) {
        out.error = proc.launchFailed ? proc.stdErr
                  : proc.timedOut     ? "The motion estimator timed out."
                                      : "Estimator exited " +
                                            std::to_string(proc.exitCode) + ": " +
                                            proc.stdErr;
        return out;
    }

    std::istringstream dump{proc.stdOut};
    const auto moves = MoveDumpParser::parse(dump, diags);
    if (diags.hasErrors()) {
        out.error = firstError(diags);
        return out;
    }

    out.analysis = analyseFlow(moves);
    out.plan = planTemperature(out.analysis, out.settings.extruder, out.settings.filament,
                               diags, {}, out.scan.layers);
    if (diags.hasErrors()) {
        out.error = firstError(diags);
        return out;
    }

    out.warning = firstWarning(diags);
    out.ok = true;
    return out;
}

} // namespace

int runServe(unsigned short port, const std::filesystem::path& exeDir,
             const std::function<std::filesystem::path(const std::filesystem::path&)>& findEstimator)
{
    std::printf("\n  %s %s\n", kProductName.data(), kVersion.data());
    std::printf("  Open  http://127.0.0.1:%u  in your browser.\n", port);
    std::printf("  Listening on loopback only. Press Ctrl+C to stop.\n\n");

    const bool ok = serve(port, [&](const Request& r) -> Response {
        if (r.path == "/" || r.path == "/index.html") {
            return Response::html(std::string(kPage));
        }

        if (r.path == "/api/version") {
            return Response::json("{\"version\":\"" + std::string(kVersion) + "\"}");
        }

        if (r.path == "/api/browse") {
            const std::string chosen = chooseGcodeFile();
            if (chosen.empty()) {
                return Response::json("{\"cancelled\":true}");
            }
            return Response::json("{\"path\":\"" + jsonEscape(chosen) + "\"}");
        }

        if (r.path == "/api/analyze" || r.path == "/api/process") {
            const auto run = runToPlan(r, exeDir, findEstimator);
            if (!run.ok) {
                return Response::error(400, run.error);
            }

            if (r.path == "/api/process") {
                std::string outPath = r.field("out");
                if (outPath.empty()) {
                    outPath = r.field("path");
                }

                std::error_code ec;
                const auto scratch =
                    std::filesystem::temp_directory_path(ec) /
                    ("sb53-out-" + std::to_string(uniqueSuffix()));
                std::filesystem::create_directories(scratch, ec);
                const auto staged = scratch / "out.gcode";

                DiagnosticList diags;
                NullProgressSink progress;
                RewriteStats stats;
                {
                    std::ifstream in{r.field("path"), std::ios::binary};
                    std::ofstream outFile{staged, std::ios::binary};
                    stats = rewriteGcode(in, outFile, run.scan, run.analysis, run.plan,
                                         run.settings.extruder, run.settings.filament,
                                         diags, progress, {});
                }
                if (diags.hasErrors()) {
                    std::filesystem::remove_all(scratch, ec);
                    return Response::error(400, firstError(diags));
                }
                std::filesystem::copy_file(
                    staged, outPath, std::filesystem::copy_options::overwrite_existing, ec);
                std::filesystem::remove_all(scratch, ec);
                if (ec) {
                    return Response::error(500, "Could not write " + outPath + ": " +
                                                    ec.message());
                }

                std::string json = "{\"output\":\"" + jsonEscape(outPath) + "\"";
                json += ",\"temperatureCommands\":" +
                        std::to_string(stats.temperatureCommands);
                json += ",\"feedratesReduced\":" +
                        std::to_string(stats.feedratesReduced);
                json += "}";
                return Response::json(json);
            }

            // --- analyze ---
            const auto& S = run.analysis.seconds;
            const auto& plan = run.plan;

            std::string json = "{";
            json += "\"moves\":" + std::to_string(run.analysis.moves.size());
            json += ",\"time\":" + jsonNumber(run.analysis.totalTime);
            json += ",\"filament\":" + jsonNumber(run.analysis.totalFilament);
            json += ",\"peakFlow\":" + jsonNumber(run.analysis.peakFlow);
            json += ",\"initialTemp\":" + jsonNumber(plan.initialTemperature);
            json += ",\"minTemp\":" + jsonNumber(plan.minTemperature);
            json += ",\"maxTemp\":" + jsonNumber(plan.maxTemperature);
            json += ",\"coolBelow\":" +
                    jsonNumber(run.settings.extruder.layerCoolingEnabled()
                                   ? run.settings.extruder.coolingLayerTime
                                   : 0.0);

            double sum = 0.0;
            for (const auto& s : S) { sum += s.averageFlow; }
            json += ",\"meanFlow\":" +
                    jsonNumber(S.empty() ? 0.0 : sum / static_cast<double>(S.size()));

            json += ",\"seconds\":[";
            for (std::size_t i = 0; i < S.size(); ++i) {
                if (i) { json += ','; }
                json += "{\"t\":" + jsonNumber(S[i].time);
                json += ",\"flow\":" + jsonNumber(S[i].averageFlow);
                json += ",\"temp\":" +
                        jsonNumber(i < plan.achievableTemperature.size()
                                       ? plan.achievableTemperature[i] : 0.0);
                json += ",\"drop\":" +
                        jsonNumber(i < plan.layerCoolingDrop.size()
                                       ? plan.layerCoolingDrop[i] : 0.0);
                json += '}';
            }
            json += "]";

            const auto durations = computeLayerDurations(run.scan.layers, S);
            json += ",\"layerCount\":" + std::to_string(durations.size());
            json += ",\"layers\":[";
            for (std::size_t i = 0; i < durations.size(); ++i) {
                if (i) { json += ','; }
                json += "{\"duration\":" + jsonNumber(durations[i]) + "}";
            }
            json += "]";

            auto sorted = durations;
            std::sort(sorted.begin(), sorted.end());
            json += ",\"medianLayer\":" +
                    jsonNumber(sorted.empty() ? 0.0 : sorted[sorted.size() / 2]);

            if (!run.warning.empty()) {
                json += ",\"warning\":\"" + jsonEscape(run.warning) + "\"";
            }
            json += "}";
            return Response::json(json);
        }

        return Response::error(404, "Not found");
    });

    return ok ? 0 : 1;
}

} // namespace sb53::web
