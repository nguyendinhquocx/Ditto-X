const axios = require('axios');
const fs = require('fs/promises');
const { env } = require('process');

// Fork-safe publish (2026-11-05): luôn publish vào repo của workflow run
// (GITHUB_REPOSITORY). Không bao giờ hard-code upstream sabrogden/Ditto —
// push master của fork chỉ đụng repo fork.
const REPO = env.GITHUB_REPOSITORY;
const API_BASE = `https://api.github.com/repos/${REPO}`;
const API_UPLOAD_BASE = `https://uploads.github.com/repos/${REPO}`;
const NIGHTLY_TAG = 'nightly';

if (!REPO) {
    console.error('GITHUB_REPOSITORY is not set; refusing to publish to a hardcoded repo.');
    process.exit(1);
}

async function GetNightlyRelease() {
    try {
        const res = await axios({
            method: 'get',
            headers: { 'X-GitHub-Api-Version': '2022-11-28', 'Authorization': `token ${env.token}` },
            url: `${API_BASE}/releases/tags/${NIGHTLY_TAG}`
        });
        return res.data;
    } catch (err) {
        if (err.response && err.response.status === 404) {
            return undefined;
        }
        throw err;
    }
}

async function CreateNightlyRelease() {
    console.log(`Creating ${NIGHTLY_TAG} release on ${REPO}`);
    const res = await axios({
        method: 'post',
        headers: { 'X-GitHub-Api-Version': '2022-11-28', 'Authorization': `token ${env.token}` },
        url: `${API_BASE}/releases`,
        data: {
            tag_name: NIGHTLY_TAG,
            target_commitish: env.GITHUB_SHA,
            name: 'Ditto-X nightly',
            body: 'Automated nightly build from master.',
            prerelease: true
        }
    });
    return res.data;
}

async function DeleteOldAssets(nightlyRelease) {
    for (const asset of nightlyRelease.assets) {
        console.log(`Deleting file ${asset.name}, id: ${asset.id}`);
        await axios({
            method: 'DELETE',
            headers: { 'X-GitHub-Api-Version': '2022-11-28', 'Authorization': `token ${env.token}` },
            url: `${API_BASE}/releases/assets/${asset.id}`
        });
    }
}

async function UploadFiles(nightlyRelease) {
    const files = await fs.readdir(env.uploadPath);
    for (const file of files) {
        if (!file.startsWith('Ditto')) {
            continue;
        }

        const fullPath = `${env.uploadPath}${file}`;
        const fileBytes = await fs.readFile(fullPath);

        console.log(`Uploading file ${fullPath}`);
        await axios({
            method: 'POST',
            headers: {
                'X-GitHub-Api-Version': '2022-11-28',
                'Authorization': `token ${env.token}`,
                'Content-Type': 'application/octet-stream',
                'Content-Length': fileBytes.length
            },
            url: `${API_UPLOAD_BASE}/releases/${nightlyRelease.id}/assets?name=${file}`,
            data: fileBytes
        });
    }
}

async function GenerateReleaseNotes() {
    try {
        const res = await axios({
            method: 'POST',
            headers: { 'X-GitHub-Api-Version': '2022-11-28', 'Authorization': `token ${env.token}` },
            url: `${API_BASE}/releases/generate-notes`,
            data: {
                tag_name: env.tag,
                previous_tag_name: env.previous_tag
            }
        });
        return res.data.body;
    } catch (err) {
        console.log(`generate-notes failed (${err.response ? err.response.status : err.message}); keeping default body`);
        return undefined;
    }
}

async function UpdateReleaseNotes(nightlyRelease) {
    const generated = await GenerateReleaseNotes();
    const body = generated || nightlyRelease.body || 'Automated nightly build from master.';

    console.log('Updating release notes');
    await axios({
        method: 'PATCH',
        headers: { 'X-GitHub-Api-Version': '2022-11-28', 'Authorization': `token ${env.token}` },
        url: `${API_BASE}/releases/${nightlyRelease.id}`,
        data: { body }
    });
}

async function Run() {
    let nightlyRelease = await GetNightlyRelease();

    if (!nightlyRelease) {
        nightlyRelease = await CreateNightlyRelease();
    }

    await DeleteOldAssets(nightlyRelease);
    await UploadFiles(nightlyRelease);
    await UpdateReleaseNotes(nightlyRelease);
}

Run();
