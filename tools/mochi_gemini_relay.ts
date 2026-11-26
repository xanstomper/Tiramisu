// mochi_gemini_relay.ts — OpenAI-compatible /v1/chat/completions that reuses
// Mochi's own Antigravity (Google OAuth) provider and keyring credentials, so
// the Xbox Hermes guest (plain HTTP, no TLS) can use Gemini 3.8 Flash with real
// tool calls. Run:  bun tools/mochi_gemini_relay.ts [--port 8790]
import { createAntigravityProvider } from '/home/jewboy420/mochi/src/model/antigravity.ts';
import { loadKeyring, getKeyringKey } from '/home/jewboy420/mochi/src/model/keyring.ts';

const DEFAULT_MODEL = process.env.HERMES_MODEL || 'gemini-3.8-flash';
const port = Number(process.argv[process.argv.indexOf('--port') + 1]) || 8790;
const key = getKeyringKey(loadKeyring(), 'antigravity');
if (!key) { console.error('no antigravity credential in Mochi keyring'); process.exit(1); }

const providers = new Map<string, ReturnType<typeof createAntigravityProvider>>();
function provider(model: string) {
  let p = providers.get(model);
  if (!p) {
    p = createAntigravityProvider({ baseUrl: '', apiKey: key, model, providerName: 'antigravity' });
    providers.set(model, p);
  }
  return p;
}

function toTools(oa: any[] = []) {
  return oa.map((t) => {
    const f = t.function ?? t;
    const props = f.parameters?.properties ?? {};
    const req: string[] = f.parameters?.required ?? [];
    return {
      name: f.name,
      description: f.description ?? '',
      parameters: Object.entries<any>(props).map(([name, s]) => ({
        name,
        type: ['string', 'number', 'integer', 'boolean', 'array'].includes(s.type) ? s.type : 'string',
        description: s.description ?? '',
        required: req.includes(name),
        ...(s.items ? { items: s.items } : {}),
      })),
    };
  });
}

function toMessages(ms: any[] = []) {
  return ms.map((m) => ({
    role: m.role,
    content: typeof m.content === 'string' || Array.isArray(m.content) ? m.content : m.content ?? '',
    ...(m.tool_calls ? { tool_calls: m.tool_calls } : {}),
    ...(m.tool_call_id ? { tool_call_id: m.tool_call_id } : {}),
    ...(m.name ? { name: m.name } : {}),
  }));
}

const json = (status: number, body: unknown) =>
  new Response(JSON.stringify(body), { status, headers: { 'content-type': 'application/json' } });

Bun.serve({
  port,
  hostname: '0.0.0.0',
  idleTimeout: 120,
  async fetch(req) {
    const url = new URL(req.url);
    if (url.pathname === '/health') return json(200, { ok: true, mock: false, upstream: 'mochi-antigravity', model: DEFAULT_MODEL });
    if (url.pathname === '/v1/models') return json(200, { data: [{ id: DEFAULT_MODEL, object: 'model' }] });
    if (url.pathname !== '/v1/chat/completions' || req.method !== 'POST') return json(404, { error: 'not found' });
    try {
      const body: any = await req.json();
      // The Xbox config may still say model=hermes/glm; anything non-gemini maps to the default.
      const model = typeof body.model === 'string' && body.model.startsWith('gemini') ? body.model : DEFAULT_MODEL;
      const r = await provider(model).chat(toMessages(body.messages), toTools(body.tools));
      const calls = r.toolCalls ?? [];
      const message: any = { role: 'assistant', content: r.content ?? (calls.length ? null : '') };
      if (calls.length) message.tool_calls = calls;
      console.log(`[relay] ${model} -> ${calls.length ? calls.map((c) => c.function.name).join(',') : (r.content ?? '').slice(0, 60)}`);
      return json(200, {
        id: `chatcmpl-${Date.now()}`,
        object: 'chat.completion',
        created: Math.floor(Date.now() / 1000),
        model,
        choices: [{ index: 0, message, finish_reason: calls.length ? 'tool_calls' : 'stop' }],
        usage: {
          prompt_tokens: r.usage?.promptTokens ?? 0,
          completion_tokens: r.usage?.completionTokens ?? 0,
          total_tokens: r.usage?.totalTokens ?? 0,
        },
      });
    } catch (e) {
      console.error('[relay] error', (e as Error).message);
      return json(502, { error: { message: (e as Error).message, type: 'upstream_error' } });
    }
  },
});
console.log(`mochi-gemini relay on :${port} (default model ${DEFAULT_MODEL})`);
