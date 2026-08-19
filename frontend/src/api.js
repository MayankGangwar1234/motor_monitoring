export const API_URL = import.meta.env.VITE_API_URL || "http://localhost:3000";

export async function fetchJson(path, options) {
  const response = await fetch(`${API_URL}${path}`, options);
  if (!response.ok) {
    const detail = await response.text();
    throw new Error(`${response.status}: ${detail}`);
  }
  return response.json();
}

