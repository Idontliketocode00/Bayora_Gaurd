# Local LLM (third-party Ollama; not our code). The model is pulled at BUILD time
# so the runtime container can sit on an internal network with no internet.
FROM ollama/ollama:latest
RUN ollama serve & pid=$!; sleep 8; ollama pull tinyllama; kill $pid; sleep 2
EXPOSE 11434
