#!/usr/bin/env python3
"""Check the recorded C renderer preview and save representative UI images."""
import argparse
import asyncio
import json
from pathlib import Path
import re

from playwright.async_api import async_playwright

ROOT = Path(__file__).resolve().parents[1]
SELECTED = (
    'Saved profile browser with maker labels',
    'Diff actions: Save profile',
    'Diff saved as TOTO plus automatic number',
    'Save capture set: existing maker picker',
)


async def check(args):
    args.artifacts.mkdir(parents=True, exist_ok=True)
    async with async_playwright() as p:
        browser = await p.chromium.launch(headless=True)
        page = await browser.new_page(viewport={'width': 1000, 'height': 900})
        errors, network, overflows = [], [], []
        page.on('pageerror', lambda error: errors.append(str(error)))
        page.on('request', lambda request: network.append(request.url)
                if request.url.startswith(('http://', 'https://')) else None)
        await page.goto(args.preview.resolve().as_uri())
        await page.wait_for_selector('#display text')
        titles = await page.locator('#screen option').all_text_contents()
        assert all(title in titles for title in SELECTED)
        assert 'IR Fuzz Compare 0.17' in await page.title()
        for index, title in enumerate(titles):
            await page.select_option('#screen', str(index))
            clipped = await page.locator('#display text').evaluate_all('''nodes => nodes
                .filter(node => node.textContent.trim())
                .map(node => ({text:node.textContent, box:node.getBBox()}))
                .filter(({box}) => box.x < -1 || box.x + box.width > 129 ||
                                  box.y < -1 || box.y + box.height > 65)
                .map(({text}) => text)''')
            if clipped:
                overflows.append({'screen': title, 'text': clipped})
            if title in SELECTED:
                text = await page.locator('#text').text_content()
                if title == SELECTED[0]:
                    assert re.search(r'(Unknown|LIXIL / INAX|TOTO) \d{3}', text)
                elif title == SELECTED[1]:
                    assert 'Save profile' in text
                elif title == SELECTED[2]:
                    assert re.search(r'Saved TOTO \d{3}', text) and '5 signals on SD' in text
                else:
                    assert 'Who is the maker?' in text and '2 captures' in text
                name = re.sub(r'[^a-z0-9]+', '-', title.lower()).strip('-')
                await page.locator('#display').screenshot(path=str(args.artifacts / (name + '.png')))
        await page.select_option('#screen', '0')
        await page.locator('#next').click()
        assert await page.locator('#screen').input_value() == '1'
        await page.locator('#previous').click()
        assert await page.locator('#screen').input_value() == '0'
        result = {
            'app_version': '0.17', 'snapshots': len(titles),
            'selected_screens': list(SELECTED), 'layout_overflows': overflows,
            'page_errors': errors, 'network_requests': network,
            'checks': 'All recorded C renderer screens fit the LCD preview; numbered maker labels, diff save action/result, shared maker picker and preview navigation checked.',
            'hardware': 'Recorded host rendering only; device fonts are approximate; no physical IR emitted.',
        }
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(result, indent=2) + '\n')
        await browser.close()
        assert not errors and not network and not overflows, result
        print(json.dumps(result, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--preview', type=Path, default=ROOT / 'simulation/preview.html')
    parser.add_argument('--report', type=Path, default=ROOT / 'simulation/v0_17_browser-report.json')
    parser.add_argument('--artifacts', type=Path, default=Path('/tmp/ir-lab-v017-ui'))
    asyncio.run(check(parser.parse_args()))
