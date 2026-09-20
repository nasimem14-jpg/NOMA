// supabase/functions/noma-chat/index.ts
//
// Flujo (adaptado para el CrowPanel 1.46" -- sin altavoz):
//   1. El reloj sube un audio (WAV, capturado del micrófono PDM) por POST junto a un device_id.
//   2. Se transcribe con Whisper (OpenAI).
//   3. Se manda la pregunta + historial a gpt-6-astra con la personalidad de Noma.
//   4. Se devuelve la respuesta como JSON de texto, para mostrarla en la pantalla redonda.
//
// DESPLIEGUE:
//   supabase functions deploy noma-chat
//   supabase secrets set OPENAI_API_KEY=tu_clave
//
// TABLA NECESARIA (ejecutar una vez en el SQL editor de Supabase):
//
//   create table noma_historial (
//     id bigint generated always as identity primary key,
//     device_id text not null,
//     role text not null,
//     content text not null,
//     created_at timestamp with time zone default now()
//   );

import { createClient } from "https://esm.sh/@supabase/supabase-js@2";

const OPENAI_API_KEY = Deno.env.get("OPENAI_API_KEY")!;
const SUPABASE_URL = Deno.env.get("SUPABASE_URL")!;
const SUPABASE_SERVICE_ROLE_KEY = Deno.env.get("SUPABASE_SERVICE_ROLE_KEY")!;

const supabase = createClient(SUPABASE_URL, SUPABASE_SERVICE_ROLE_KEY);

const SYSTEM_PROMPT = `Eres NOMA, una asistente de IA con espíritu investigador: curiosa,
analítica y meticulosa antes de responder. Tu trato es amable, cercano y humilde -- nunca
arrogante, y reconoces con naturalidad cuando algo no lo sabes con certeza. Tu punto fuerte
es la toma de decisiones: cuando te preguntan qué hacer, sopesas las opciones con cuidado y
das una recomendación clara y bien razonada, no una lista ambigua de posibilidades.
Respondes siempre en español, de forma breve (2-3 frases cortas), porque tu respuesta se
muestra como texto en una pantalla redonda pequeña de un reloj -- nada de listas, markdown
ni párrafos largos.`;

const MAX_TURNOS_HISTORIAL = 10; // cuántos mensajes atrás recordar

Deno.serve(async (req) => {
  try {
    const formData = await req.formData();
    const audioFile = formData.get("audio") as File;
    const deviceId = (formData.get("device_id") as string) ?? "reloj-1";

    if (!audioFile) {
      return new Response(JSON.stringify({ error: "Falta el archivo de audio" }), { status: 400 });
    }

    // 1. Transcribir el audio con Whisper
    const whisperForm = new FormData();
    whisperForm.append("file", audioFile, "audio.wav");
    whisperForm.append("model", "whisper-1");
    whisperForm.append("language", "es");

    const whisperRes = await fetch("https://api.openai.com/v1/audio/transcriptions", {
      method: "POST",
      headers: { Authorization: `Bearer ${OPENAI_API_KEY}` },
      body: whisperForm,
    });
    const whisperData = await whisperRes.json();
    const pregunta: string = whisperData.text?.trim();

    if (!pregunta) {
      return new Response(JSON.stringify({ respuesta: "No te he entendido, prueba otra vez." }), { status: 200 });
    }

    // 2. Recuperar historial reciente de este reloj
    const { data: historialPrevio } = await supabase
      .from("noma_historial")
      .select("role, content")
      .eq("device_id", deviceId)
      .order("created_at", { ascending: false })
      .limit(MAX_TURNOS_HISTORIAL);

    const historial = (historialPrevio ?? []).reverse();

    // 3. Preguntar a Noma (gpt-6-astra)
    const chatRes = await fetch("https://api.openai.com/v1/chat/completions", {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        Authorization: `Bearer ${OPENAI_API_KEY}`,
      },
      body: JSON.stringify({
        model: "gpt-6-astra",
        max_tokens: 200,
        messages: [
          { role: "system", content: SYSTEM_PROMPT },
          ...historial,
          { role: "user", content: pregunta },
        ],
      }),
    });
    const chatData = await chatRes.json();
    const respuesta: string = chatData.choices[0].message.content;

    // 4. Guardar los dos turnos nuevos en el historial
    await supabase.from("noma_historial").insert([
      { device_id: deviceId, role: "user", content: pregunta },
      { device_id: deviceId, role: "assistant", content: respuesta },
    ]);

    // 5. Devolver el texto para que el reloj lo muestre en pantalla
    return new Response(JSON.stringify({ pregunta, respuesta }), {
      status: 200,
      headers: { "Content-Type": "application/json" },
    });
  } catch (err) {
    return new Response(JSON.stringify({ error: String(err) }), { status: 500 });
  }
});
