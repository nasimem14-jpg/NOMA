
import { createClient } from "https://esm.sh/@supabase/supabase-js@2";

const OPENAI_API_KEY = Deno.env.get("OPENAI_API_KEY");
const SUPABASE_URL = Deno.env.get("SUPABASE_URL");
const SUPABASE_SERVICE_ROLE_KEY = Deno.env.get(
  "SUPABASE_SERVICE_ROLE_KEY"
);

if (
  !OPENAI_API_KEY ||
  !SUPABASE_URL ||
  !SUPABASE_SERVICE_ROLE_KEY
) {
  throw new Error("Faltan secretos de configuración del servidor");
}

const supabase = createClient(
  SUPABASE_URL,
  SUPABASE_SERVICE_ROLE_KEY
);

const OPENAI_URL = "https://api.openai.com/v1";
const CHAT_MODEL = "gpt-5.6-luna";
const MAX_TURNOS_HISTORIAL = 10;
const MAX_AUDIO_BYTES = 10 * 1024 * 1024;

const SYSTEM_PROMPT = `
Eres NOMA, una asistente de IA con espíritu investigador:
curiosa, analítica y meticulosa antes de responder.

Tu trato es amable, cercano y humilde.
Reconoces con naturalidad cuando no sabes algo con certeza.

Cuando el usuario pide ayuda para decidir, explica brevemente
las opciones y sus consecuencias sin imponer una decisión.

Respondes siempre en español.
Tu respuesta debe ser breve, de 2 o 3 frases cortas,
porque se mostrará en la pantalla pequeña de un reloj.

No uses listas, Markdown ni párrafos largos.
`;

function jsonResponse(
  body: Record<string, unknown>,
  status = 200
): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: {
      "Content-Type": "application/json; charset=utf-8",
      "Access-Control-Allow-Origin": "*",
      "Access-Control-Allow-Headers": "authorization, x-client-info, apikey, content-type",
    },
  });
}

function getOpenAIError(data: any): string {
  return (
    data?.error?.message ||
    data?.message ||
    "Error desconocido de OpenAI"
  );
}

Deno.serve(async (req: Request): Promise<Response> => {
  if (req.method === "OPTIONS") {
    return new Response("ok", {
      headers: {
        "Access-Control-Allow-Origin": "*",
        "Access-Control-Allow-Headers":
          "authorization, x-client-info, apikey, content-type",
      },
    });
  }

  if (req.method !== "POST") {
    return jsonResponse(
      { error: "Método no permitido. Usa POST." },
      405
    );
  }

  try {
    const formData = await req.formData();

    const audioValue = formData.get("audio");
    const deviceIdValue = formData.get("device_id");

    if (!(audioValue instanceof File)) {
      return jsonResponse(
        { error: "Falta un archivo de audio válido." },
        400
      );
    }

    if (audioValue.size === 0) {
      return jsonResponse(
        { error: "El archivo de audio está vacío." },
        400
      );
    }

    if (audioValue.size > MAX_AUDIO_BYTES) {
      return jsonResponse(
        { error: "El archivo de audio es demasiado grande." },
        413
      );
    }

    const deviceId =
      typeof deviceIdValue === "string" &&
      deviceIdValue.trim().length > 0
        ? deviceIdValue.trim()
        : "reloj-1";

    if (deviceId.length > 100) {
      return jsonResponse(
        { error: "El device_id es demasiado largo." },
        400
      );
    }

    // 1. Transcribir el audio con OpenAI
    const whisperForm = new FormData();

    whisperForm.append(
      "file",
      audioValue,
      audioValue.name || "audio.wav"
    );

    whisperForm.append("model", "whisper-1");
    whisperForm.append("language", "es");
    whisperForm.append("response_format", "json");

    const whisperResponse = await fetch(
      `${OPENAI_URL}/audio/transcriptions`,
      {
        method: "POST",
        headers: {
          Authorization: `Bearer ${OPENAI_API_KEY}`,
        },
        body: whisperForm,
      }
    );

    const whisperData = await whisperResponse.json();

    if (!whisperResponse.ok) {
      console.error("Error de Whisper:", whisperData);

      return jsonResponse(
        {
          error: "No se pudo transcribir el audio.",
          details: getOpenAIError(whisperData),
        },
        502
      );
    }

    const pregunta =
      typeof whisperData?.text === "string"
        ? whisperData.text.trim()
        : "";

    if (!pregunta) {
      return jsonResponse({
        respuesta: "No te he entendido. Prueba otra vez.",
      });
    }

    // 2. Recuperar el historial del dispositivo
    const {
      data: historialPrevio,
      error: historialError,
    } = await supabase
      .from("noma_historial")
      .select("role, content")
      .eq("device_id", deviceId)
      .order("created_at", { ascending: false })
      .limit(MAX_TURNOS_HISTORIAL);

    if (historialError) {
      console.error("Error al leer el historial:", historialError);

      return jsonResponse(
        { error: "No se pudo recuperar el historial." },
        500
      );
    }

    const historial = (historialPrevio ?? [])
      .reverse()
      .filter(
        (item) =>
          (item.role === "user" || item.role === "assistant") &&
          typeof item.content === "string"
      );

    // 3. Enviar la pregunta al modelo
    const chatResponse = await fetch(
      `${OPENAI_URL}/chat/completions`,
      {
        method: "POST",
        headers: {
          "Content-Type": "application/json",
          Authorization: `Bearer ${OPENAI_API_KEY}`,
        },
        body: JSON.stringify({
          model: CHAT_MODEL,
          max_tokens: 200,
          messages: [
            {
              role: "system",
              content: SYSTEM_PROMPT,
            },
            ...historial,
            {
              role: "user",
              content: pregunta,
            },
          ],
        }),
      }
    );

    const chatData = await chatResponse.json();

    if (!chatResponse.ok) {
      console.error("Error del modelo:", chatData);

      return jsonResponse(
        {
          error: "No se pudo obtener una respuesta de NOMA.",
          details: getOpenAIError(chatData),
        },
        502
      );
    }

    const respuesta =
      chatData?.choices?.[0]?.message?.content?.trim();

    if (!respuesta) {
      console.error("Respuesta inesperada del modelo:", chatData);

      return jsonResponse(
        { error: "El modelo devolvió una respuesta vacía." },
        502
      );
    }

    // 4. Guardar la conversación
    const { error: insertError } = await supabase
      .from("noma_historial")
      .insert([
        {
          device_id: deviceId,
          role: "user",
          content: pregunta,
        },
        {
          device_id: deviceId,
          role: "assistant",
          content: respuesta,
        },
      ]);

    if (insertError) {
      console.error("Error al guardar historial:", insertError);

      // La respuesta se puede devolver aunque falle el historial.
      return jsonResponse({
        pregunta,
        respuesta,
        historial_guardado: false,
      });
    }

    // 5. Devolver la respuesta al reloj
    return jsonResponse({
      pregunta,
      respuesta,
      historial_guardado: true,
    });
  } catch (error) {
    console.error("Error interno de NOMA:", error);

    return jsonResponse(
      { error: "Error interno del servidor." },
      500
    );
  }
});
