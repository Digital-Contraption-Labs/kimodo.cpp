// Extend the compact canvas viewer with model selection and a prompt sequence.
window.addEventListener('load', async () => {
  const prompt = document.querySelector('#prompt');
  const form = prompt?.closest('.promptbox');
  const generate = document.querySelector('#generate');
  if (!prompt || !form || !generate) return;
  const [models, quantizations] = await Promise.all([
    fetch('/api/models').then(r => r.json()),
    fetch('/api/text-quantizations').then(r => r.json()),
  ]);
  window.kimodoModels = models;
  window.kimodoQuantizations = quantizations;

  const modelLabel = document.createElement('label');
  modelLabel.htmlFor = 'motionModel'; modelLabel.textContent = 'Motion model';
  const select = document.createElement('select');
  select.id = 'motionModel'; select.style.cssText = 'width:100%;padding:10px;border-radius:10px;background:#0b1015;color:#f3f6f4;border:1px solid #25313b';
  for (const model of models) {
    const option = document.createElement('option'); option.value = model.id;
    option.disabled = !model.available;
    option.textContent = `${model.label}${model.available ? '' : ' — coming soon'}`;
    select.append(option);
  }
  const modelHint = document.createElement('div'); modelHint.className = 'hint';
  const updateModel = () => {
    const model = models.find(item => item.id === select.value);
    if (!model) return;
    const title = document.querySelector('.eyebrow');
    if (title) title.textContent = `${model.label} · Vulkan`;
    modelHint.classList.toggle('license-warning', !model.commercial);
    const terms = model.commercial ? 'commercial use permitted under NVIDIA Open Model License' : '⚠ non-commercial research use only';
    const detail = model.available ? `${model.skeleton} · ${model.upstream} · ` : `${model.skeleton} · ${model.reason} · `;
    const link = document.createElement('a'); link.href = model.license_url; link.target = '_blank'; link.rel = 'noreferrer'; link.textContent = terms;
    modelHint.replaceChildren(document.createTextNode(detail), link);
  };
  select.onchange = updateModel;
  form.insertBefore(modelLabel, prompt); form.insertBefore(select, prompt); form.insertBefore(modelHint, prompt); updateModel();

  const quantizationLabel = document.createElement('label');
  quantizationLabel.htmlFor = 'textQuantization'; quantizationLabel.textContent = 'Text encoder quantization';
  const quantizationSelect = document.createElement('select');
  quantizationSelect.id = 'textQuantization'; quantizationSelect.style.cssText = select.style.cssText;
  for (const quantization of quantizations) {
    const option = document.createElement('option'); option.value = quantization.id;
    option.disabled = !quantization.available;
    option.textContent = `${quantization.label}${quantization.available ? '' : ' — unavailable'}`;
    quantizationSelect.append(option);
  }
  if (quantizations.some(quantization => quantization.id === 'q8_0' && quantization.available)) {
    quantizationSelect.value = 'q8_0';
  }
  const quantizationHint = document.createElement('div'); quantizationHint.className = 'hint';
  const updateQuantization = () => {
    const quantization = quantizations.find(item => item.id === quantizationSelect.value);
    if (!quantization) return;
    const size = quantization.bytes ? `${(quantization.bytes / 1073741824).toFixed(2)} GiB · ` : '';
    quantizationHint.textContent = quantization.available
      ? `${size}${quantization.description}`
      : quantization.reason;
  };
  quantizationSelect.onchange = updateQuantization;
  form.insertBefore(quantizationLabel, prompt); form.insertBefore(quantizationSelect, prompt); form.insertBefore(quantizationHint, prompt); updateQuantization();

  const sequence = document.createElement('div');
  sequence.style.cssText = 'display:grid;gap:10px;width:100%';
  prompt.before(sequence);
  prompt.classList.add('sequence-prompt');
  const autoGrow = area => {
    area.style.height = '0px';
    const minimum = Number.parseFloat(getComputedStyle(area).minHeight) || 0;
    area.style.height = `${Math.max(minimum, area.scrollHeight)}px`;
  };
  prompt.style.minHeight = '110px';
  prompt.addEventListener('input', () => autoGrow(prompt));
  const minFrames = 60, maxFrames = 360;
  const segmentControls = new Map();
  const validFrames = value => Number.isInteger(value) && value >= minFrames && value <= maxFrames;
  const clampFrames = value => validFrames(Number(value)) ? Number(value) : Math.max(minFrames, Math.min(maxFrames, Number(value) || 150));
  const configureDuration = (duration, frames) => {
    duration.type = 'number'; duration.min = String(minFrames); duration.max = String(maxFrames); duration.step = '1'; duration.value = String(clampFrames(frames));
    duration.title = `Frames (${minFrames}–${maxFrames})`;
    duration.addEventListener('change', () => { duration.value = String(clampFrames(duration.value)); duration.setCustomValidity(''); });
    duration.addEventListener('invalid', () => duration.setCustomValidity(`Use a whole number from ${minFrames} to ${maxFrames} frames.`));
  };
  const primaryRow = document.createElement('div');
  primaryRow.style.cssText = 'display:grid;grid-template-columns:1fr 74px;gap:7px;align-items:start';
  const primaryDuration = document.createElement('input');
  configureDuration(primaryDuration, 150);
  primaryRow.append(prompt, primaryDuration); sequence.append(primaryRow);
  segmentControls.set(primaryRow, primaryDuration);
  autoGrow(prompt);
  const count = document.createElement('div'); count.className = 'hint';
  const updateCount = () => {
    const prompts = sequence.querySelectorAll('.sequence-prompt');
    count.textContent = `${prompts.length} segment${prompts.length === 1 ? '' : 's'} · 5-frame conditioned hand-off`;
  };
  const addSegment = (text = '', frames = 150) => {
    const row = document.createElement('div'); row.style.cssText = 'display:grid;grid-template-columns:1fr 74px auto;gap:7px;align-items:start';
    const textArea = document.createElement('textarea'); textArea.className = 'sequence-prompt'; textArea.value = text;
    textArea.placeholder = 'Describe the next motion'; textArea.style.minHeight = '110px';
    textArea.style.resize = 'none'; textArea.style.overflow = 'hidden'; textArea.addEventListener('input', () => autoGrow(textArea));
    const duration = document.createElement('input'); configureDuration(duration, frames);
    const remove = document.createElement('button'); remove.type = 'button'; remove.textContent = '×'; remove.title = 'Remove segment'; remove.style.cssText = 'padding:8px 12px;background:#24313a;color:#dce9e8';
    remove.onclick = () => { row.remove(); updateCount(); };
    row.append(textArea, duration, remove); sequence.append(row); segmentControls.set(row, duration);
    autoGrow(textArea);
    updateCount();
  };
  const add = document.createElement('button'); add.type = 'button'; add.textContent = '+ Add prompt segment';
  add.style.cssText = 'justify-self:start;padding:8px 12px;background:#24313a;color:#dce9e8';
  add.onclick = () => addSegment(); form.insertBefore(add, generate); form.insertBefore(count, generate); updateCount();

  // Diffusion seed. The viewer used to send seed 0 for every request, so the
  // same prompt always produced the same animation. Start random, keep the
  // seed of a restored animation so it can be regenerated exactly, and offer
  // a one-click reroll for a fresh sample of the same prompt.
  const maxSeed = 0xFFFFFFFF;
  const randomSeed = () => crypto.getRandomValues(new Uint32Array(1))[0];
  const clampSeed = value => { const n = Math.floor(Number(value)); return Number.isFinite(n) && n >= 0 && n <= maxSeed ? n : randomSeed(); };
  const seedRow = document.createElement('div'); seedRow.style.cssText = 'display:grid;grid-template-columns:auto 1fr auto;gap:7px;align-items:center';
  const seedLabel = document.createElement('label'); seedLabel.htmlFor = 'seed'; seedLabel.textContent = 'Seed';
  const seedInput = document.createElement('input'); seedInput.id = 'seed'; seedInput.type = 'number'; seedInput.min = '0'; seedInput.max = String(maxSeed); seedInput.step = '1';
  seedInput.value = String(randomSeed()); seedInput.title = 'Diffusion noise seed. Same prompt + seed reproduces the same motion; change it for a different sample.';
  seedInput.addEventListener('change', () => { seedInput.value = String(clampSeed(seedInput.value)); seedInput.setCustomValidity(''); });
  seedInput.addEventListener('invalid', () => seedInput.setCustomValidity(`Use a whole number from 0 to ${maxSeed}.`));
  const reroll = document.createElement('button'); reroll.type = 'button'; reroll.textContent = 'Random'; reroll.title = 'Pick a new random seed';
  reroll.style.cssText = 'padding:8px 12px;background:#24313a;color:#dce9e8';
  reroll.onclick = () => { seedInput.value = String(randomSeed()); };
  seedRow.append(seedLabel, seedInput, reroll); form.insertBefore(seedRow, generate);

  // Text guidance (classifier-free guidance) weight. Upstream samples at 2.0.
  // Lower values follow the prompt less literally but move more naturally;
  // higher values track the caption harder and tend to look stiff.
  const defaultCFG = 2, minCFG = 0, maxCFG = 20;
  const clampCFG = value => { const n = Number(value); return Number.isFinite(n) && n >= minCFG && n <= maxCFG ? Math.round(n * 100) / 100 : defaultCFG; };
  const cfgRow = document.createElement('div'); cfgRow.style.cssText = seedRow.style.cssText;
  const cfgLabel = document.createElement('label'); cfgLabel.htmlFor = 'textCFG'; cfgLabel.textContent = 'Text guidance';
  const cfgInput = document.createElement('input'); cfgInput.id = 'textCFG'; cfgInput.type = 'number'; cfgInput.min = String(minCFG); cfgInput.max = String(maxCFG); cfgInput.step = '0.1';
  cfgInput.value = String(defaultCFG); cfgInput.title = `Classifier-free guidance weight (${minCFG}–${maxCFG}). Upstream default 2.0; lower is looser and more natural, higher follows the prompt more literally.`;
  cfgInput.addEventListener('change', () => { cfgInput.value = String(clampCFG(cfgInput.value)); cfgInput.setCustomValidity(''); });
  cfgInput.addEventListener('invalid', () => cfgInput.setCustomValidity(`Use a number from ${minCFG} to ${maxCFG}.`));
  const cfgReset = document.createElement('button'); cfgReset.type = 'button'; cfgReset.textContent = 'Default'; cfgReset.title = 'Reset to the upstream default of 2.0';
  cfgReset.style.cssText = reroll.style.cssText; cfgReset.onclick = () => { cfgInput.value = String(defaultCFG); };
  cfgRow.append(cfgLabel, cfgInput, cfgReset); form.insertBefore(cfgRow, generate);

  // Keyframe constraints.  "Pin current pose" takes the pose on screen (the
  // selected animation at the current frame) as a full-body keyframe for the
  // next generation, at the same frame by default.  Positions are absolute in
  // the clip's space, so the pose keeps its place on the ground.  Pins stay
  // while other animations are viewed; one that was generated with
  // constraints (also a client's, through the API) offers to reuse them.
  let keyframes = [], selectedConstraints = [];
  const constraintRow = document.createElement('div'); constraintRow.style.cssText = seedRow.style.cssText;
  const keyframeLabel = document.createElement('label'); keyframeLabel.textContent = 'Keyframe guidance';
  const pin = document.createElement('button'); pin.type = 'button'; pin.textContent = 'Pin current pose';
  pin.title = 'Make the next generation pass through the pose on screen'; pin.style.cssText = reroll.style.cssText;
  const constraintCFGInput = document.createElement('input'); constraintCFGInput.type = 'number'; constraintCFGInput.min = String(minCFG); constraintCFGInput.max = String(maxCFG); constraintCFGInput.step = '0.1';
  constraintCFGInput.value = String(defaultCFG); constraintCFGInput.title = `Constraint guidance weight (${minCFG}–${maxCFG}). Upstream default 2.0; raise it if keyframes are missed.`;
  constraintCFGInput.addEventListener('change', () => { constraintCFGInput.value = String(clampCFG(constraintCFGInput.value)); });
  constraintRow.append(keyframeLabel, constraintCFGInput, pin);
  const keyframeList = document.createElement('div'); keyframeList.style.cssText = 'display:grid;gap:5px';
  const keyframeHint = document.createElement('div'); keyframeHint.className = 'hint';
  const reuse = document.createElement('button'); reuse.type = 'button'; reuse.style.cssText = 'justify-self:start;padding:6px 10px;background:#24313a;color:#dce9e8';
  reuse.onclick = () => { keyframes = structuredClone(selectedConstraints); renderKeyframes(); };
  const renderKeyframes = message => {
    reuse.hidden = !selectedConstraints.length;
    reuse.textContent = `Use the selected animation's ${selectedConstraints.length} constraint${selectedConstraints.length === 1 ? '' : 's'}`;
    keyframeList.replaceChildren(...keyframes.map((constraint, index) => {
      const row = document.createElement('div'); row.style.cssText = 'display:grid;grid-template-columns:1fr 74px auto;gap:7px;align-items:center';
      const label = document.createElement('span'); label.className = 'hint'; label.textContent = constraint.type;
      const remove = document.createElement('button'); remove.type = 'button'; remove.textContent = '×'; remove.title = 'Remove constraint';
      remove.style.cssText = 'padding:6px 10px;background:#24313a;color:#dce9e8';
      remove.onclick = () => { keyframes.splice(index, 1); renderKeyframes(); };
      let frame;
      if (constraint.frame_indices?.length === 1) {
        frame = document.createElement('input'); frame.type = 'number'; frame.min = '0'; frame.step = '1'; frame.value = String(constraint.frame_indices[0]);
        frame.title = 'Frame of the generated clip this pose is reached at';
        frame.addEventListener('change', () => { constraint.frame_indices = [Math.max(0, Math.floor(Number(frame.value)) || 0)]; frame.value = String(constraint.frame_indices[0]); });
        label.textContent = `${constraint.type} at frame`;
      } else {
        frame = document.createElement('span'); frame.className = 'hint'; frame.textContent = `${constraint.frame_indices?.length ?? 0} frames`;
      }
      row.append(label, frame, remove);
      return row;
    }));
    keyframeHint.textContent = message ?? (keyframes.length
      ? 'Frames count across the whole clip. Pinned poses keep their position on the ground.'
      : 'Select an animation, scrub to a pose, and pin it to constrain the next generation.');
  };
  pin.onclick = () => {
    const pose = window.kimodoCurrentPose?.();
    if (!pose) { renderKeyframes('Select a ready animation first.'); return; }
    const skeletonOf = id => models.find(model => model.id === id)?.skeleton_key;
    if (skeletonOf(pose.model) !== skeletonOf(select.value)) {
      renderKeyframes('That pose is from a different skeleton than the selected motion model.'); return;
    }
    const rotations = [];
    for (let i = 0; i < pose.rotations.length; i += 4) rotations.push(pose.rotations.slice(i, i + 4));
    keyframes.push({type: 'fullbody', frame_indices: [pose.frame], root_positions: [pose.root], local_joints_rot_xyzw: [rotations]});
    renderKeyframes();
  };
  form.insertBefore(constraintRow, generate); form.insertBefore(keyframeList, generate); form.insertBefore(reuse, generate); form.insertBefore(keyframeHint, generate); renderKeyframes();

  // Upstream's post-processing: foot-skate cleanup and IK that lands the
  // constraints exactly.  On by default, as in NVIDIA's demo, which leaves it
  // off for the G1 robot.
  const postRow = document.createElement('label'); postRow.style.cssText = 'display:flex;gap:8px;align-items:center';
  const postInput = document.createElement('input'); postInput.type = 'checkbox'; postInput.style.cssText = 'width:auto';
  postRow.title = 'Clean up foot skating and pin constraints exactly with IK after generation';
  postRow.append(postInput, document.createTextNode('Post-process (foot contacts, exact constraints)'));
  const postDefault = () => { postInput.checked = models.find(model => model.id === select.value)?.skeleton_key !== 'g1skel34'; };
  select.addEventListener('change', postDefault); postDefault();
  form.insertBefore(postRow, generate);

  // The gallery owns the selected animation; receive its full saved sequence
  // rather than restoring only animation.prompt (the first segment).
  window.addEventListener('kimodo:restore-sequence', event => {
    const {segments, model, text_quantization: textQuantization, seed, text_cfg: textCFG, constraints, constraint_cfg: constraintCFG, post_processing: postProcessing} = event.detail || {};
    if (seed !== undefined && seed !== null) seedInput.value = String(clampSeed(seed));
    cfgInput.value = String(clampCFG(textCFG ?? defaultCFG));
    selectedConstraints = Array.isArray(constraints) ? constraints : [];
    if (selectedConstraints.length) constraintCFGInput.value = String(clampCFG(constraintCFG ?? defaultCFG));
    renderKeyframes();
    if (model && [...select.options].some(option => option.value === model)) {
      select.value = model;
      updateModel();
    }
    if (typeof postProcessing === 'boolean') postInput.checked = postProcessing;
    if (textQuantization && [...quantizationSelect.options].some(option => option.value === textQuantization && !option.disabled)) {
      quantizationSelect.value = textQuantization;
      updateQuantization();
    }
    const restored = Array.isArray(segments) && segments.length
      ? segments
      : [{prompt: prompt.value, frames: 150}];
    const first = restored[0];
    prompt.value = first.prompt || '';
    autoGrow(prompt);
    primaryDuration.value = String(clampFrames(first.frames));
    for (const row of [...sequence.children]) {
      if (row !== primaryRow) row.remove();
    }
    for (const segment of restored.slice(1)) {
      addSegment(segment.prompt || '', clampFrames(segment.frames));
    }
    updateCount();
  });

  const nativeFetch = window.fetch.bind(window);
  window.fetch = (input, init) => {
    if (typeof input === 'string' && input.endsWith('/api/generate') && init?.body) {
      const body = JSON.parse(init.body);
      body.model = select.value;
      body.text_quantization = quantizationSelect.value;
      body.transition_frames = 5;
      body.seed = clampSeed(seedInput.value); seedInput.value = String(body.seed);
      body.text_cfg = clampCFG(cfgInput.value); cfgInput.value = String(body.text_cfg);
      body.post_processing = postInput.checked;
      if (keyframes.length) {
        body.constraints = keyframes;
        body.constraint_cfg = clampCFG(constraintCFGInput.value); constraintCFGInput.value = String(body.constraint_cfg);
      }
      body.segments = [...sequence.querySelectorAll('.sequence-prompt')].map(area => {
        const row = area.closest('div');
        const duration = segmentControls.get(row);
        const frames = Number(duration?.value);
        if (!validFrames(frames)) throw new Error(`Each segment must be a whole number from ${minFrames} to ${maxFrames} frames.`);
        return {prompt: area.value, frames};
      });
      return nativeFetch(input, {...init, body: JSON.stringify(body)});
    }
    return nativeFetch(input, init);
  };
  window.dispatchEvent(new Event('kimodo:sequence-controls-ready'));
});
