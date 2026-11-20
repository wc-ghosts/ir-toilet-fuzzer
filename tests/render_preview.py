#!/usr/bin/env python3
"""Preview draw commands recorded from the actual app C renderer in host simulation."""
import argparse
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    screens = []
    for line in args.report.read_text().splitlines():
        if line.startswith('SCREEN: '):
            screens.append({'title': line[8:], 'lines': [], 'draw': []})
        elif line.startswith('| ') and screens:
            screens[-1]['lines'].append(line[2:-2].rstrip())
        elif line.startswith('DRAW: ') and screens:
            screens[-1]['draw'].append(json.loads(line[6:]))
    if not screens or not screens[0]['draw']:
        parser.error('report needs snapshots with actual renderer draw commands')
    payload = json.dumps(screens).replace('<', '\\u003c')
    html = r'''<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<title>IR Fuzz Compare 0.17 — menu preview</title>
<style>body{font:16px system-ui;background:#171b22;color:#eee;max-width:760px;margin:35px auto;padding:20px}h1{font-size:26px}p{line-height:1.5;color:#ccd2df}select,button{font:inherit;padding:10px;margin:8px 8px 16px 0;background:#303745;color:#fff;border:1px solid #697388;border-radius:6px}.device{background:#e9e9e8;border-radius:24px;padding:25px;max-width:600px}svg{display:block;width:100%;background:#d9e3a3;image-rendering:pixelated}text{font-family:monospace}pre{background:#252b35;border-radius:8px;padding:18px;overflow:auto;line-height:1.5}small{display:block;color:#b6bece;margin-top:20px;line-height:1.6}</style>
<h1>IR Fuzz Compare 0.17</h1><p>Sniff mode → capture → choose a label → save on SD and choose a maker. Profiles use automatic names such as TOTO 003. Capture sets / diff also has Save profile, using the same maker picker. Remove duplicates is an explicit profile action. Load profile → choose a named signal → fuzzer, analysis or original replay. Quick single-signal capture and the existing range editor remain available. More options includes an INAX command-inversion edit. TOTO retains its prefix and offers XOR repair. INAX retains its first byte and can maintain a captured inverse pair. Advanced protocol analysis remains optional. Saved-signal matching is passive. Live sending waits 1–2 seconds after each packet; OK pauses and saves the exact emitted waveform.</p>
<button id="previous">Previous</button><select id="screen" aria-label="Recorded app screen"></select><button id="next">Next</button>
<div class="device"><svg viewBox="0 0 128 64" role="img" aria-label="Recorded Flipper screen" id="display"></svg></div>
<pre id="text"></pre><small>Shapes, positions, labels and selection come from draw calls in the actual C renderer. Fonts and LCD colours are approximate. These are recorded screens, not a device emulator; the terminal simulator runs the actual button handlers. No physical IR was emitted.</small>
<script>const screens=PAYLOAD;const menu=document.querySelector('#screen');const svg=document.querySelector('#display');const ns='http://www.w3.org/2000/svg';screens.forEach((s,i)=>{const o=document.createElement('option');o.value=i;o.textContent=s.title;menu.append(o)});function show(){const s=screens[+menu.value];svg.replaceChildren();s.draw.forEach(d=>{let node;const colour=d.color?'#182015':'#d9e3a3';if(d.op==='b'){node=document.createElementNS(ns,'rect');node.setAttribute('x',d.x);node.setAttribute('y',d.y);node.setAttribute('width',d.x2);node.setAttribute('height',d.y2);node.setAttribute('fill',colour)}else if(d.op==='l'){node=document.createElementNS(ns,'line');node.setAttribute('x1',d.x);node.setAttribute('y1',d.y);node.setAttribute('x2',d.x2);node.setAttribute('y2',d.y2);node.setAttribute('stroke',colour);node.setAttribute('stroke-width',1)}else{node=document.createElementNS(ns,'text');node.setAttribute('x',d.x);node.setAttribute('y',d.y);node.setAttribute('fill',colour);node.setAttribute('font-size',d.font?8.3:7.5);node.setAttribute('font-weight',d.font?'bold':'normal');node.textContent=d.text}svg.append(node)});document.querySelector('#text').textContent=s.lines.join('\n')}menu.onchange=show;document.querySelector('#previous').onclick=()=>{menu.value=(+menu.value+screens.length-1)%screens.length;show()};document.querySelector('#next').onclick=()=>{menu.value=(+menu.value+1)%screens.length;show()};show();</script></html>'''
    args.output.write_text(html.replace('PAYLOAD', payload))


if __name__ == '__main__':
    main()
