"""Verificación de los tokens del Admin de pcfutbol.site en las APIs internas (foro, portal, editor, agentes).

COPIA CANÓNICA: Admin-PCFutbol/contracts/verificador/pcf_interno.py. Se copia TAL CUAL a cada servicio (un test del
Admin compara las copias). Solo depende de `cryptography`; no sabe nada de frameworks.

Token (ARCHITECTURE §6.1): JWT EdDSA (Ed25519) con cabecera {alg:"EdDSA", typ:"JWT", kid} y claims
  iss="admin.pcfutbol.site", aud=<servicio>, sub, act (nick), scope (lista), iat, exp (≤ 60 s), jti,
  htm (método), htu (ruta + query, tal cual llega), bh (SHA-256 hex del cuerpo), rid (request_id).

Configuración del servicio:
  PCF_ADMIN_FIRMA_PUB = "kid:base64-DER-SPKI[,kid2:...]"   (claves públicas; la actual y, al rotar, la anterior)
  PCF_INTERNO_JTI_DIR = carpeta para los jti usados (por defecto /dev/shm/pcf-interno-<aud>); vale con varios
                        procesos (gunicorn) porque usa O_CREAT|O_EXCL.

Uso:
    claims = verificar(cabecera_authorization, aud="foro", metodo="PATCH", htu="/internal/v1/users/7", cuerpo=b"...")
    if "forum.users:write" not in claims["scope"]: ... 403
Lanza TokenNoValido(codigo, mensaje) → responder 401 {"error": {"code": codigo, ...}} (403 si el código es SCOPE).
"""
import base64
import hashlib
import json
import os
import time

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives.serialization import load_der_public_key

ISS = "admin.pcfutbol.site"
MAX_VIDA = 60          # segundos entre iat y exp
MARGEN = 30            # desajuste de reloj tolerado
JTI_TTL = 180          # los jti se guardan más que la vida del token


class TokenNoValido(Exception):
    def __init__(self, codigo, mensaje):
        super().__init__(mensaje)
        self.codigo, self.mensaje = codigo, mensaje


def _b64d(s):
    return base64.urlsafe_b64decode(s + "=" * (-len(s) % 4))


def claves_publicas(valor=None):
    """{kid: Ed25519PublicKey} desde PCF_ADMIN_FIRMA_PUB."""
    valor = os.environ.get("PCF_ADMIN_FIRMA_PUB", "") if valor is None else valor
    out = {}
    for parte in valor.split(","):
        if ":" in parte:
            kid, der = parte.strip().split(":", 1)
            out[kid] = load_der_public_key(base64.b64decode(der))
    return out


def _jti_nuevo(aud, jti):
    """True si el jti no se había visto (y lo apunta). Atómico entre procesos."""
    d = os.environ.get("PCF_INTERNO_JTI_DIR") or f"/dev/shm/pcf-interno-{aud}"
    os.makedirs(d, mode=0o700, exist_ok=True)
    ahora = time.time()
    try:   # limpieza barata de vez en cuando
        if int(ahora) % 20 == 0:
            for f in os.scandir(d):
                if f.stat().st_mtime < ahora - JTI_TTL:
                    os.unlink(f.path)
    except OSError:
        pass
    try:
        os.close(os.open(os.path.join(d, jti), os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600))
        return True
    except FileExistsError:
        return False


def verificar(autorizacion, *, aud, metodo, htu, cuerpo=b"", claves=None, ahora=None):
    if not autorizacion or not autorizacion.startswith("Bearer "):
        raise TokenNoValido("SIN_TOKEN", "Falta el token.")
    token = autorizacion[7:].strip()
    partes = token.split(".")
    if len(partes) != 3 or len(token) > 4096:
        raise TokenNoValido("TOKEN_MALFORMADO", "Token mal formado.")
    try:
        cab = json.loads(_b64d(partes[0]))
        cl = json.loads(_b64d(partes[1]))
        firma = _b64d(partes[2])
    except (ValueError, TypeError):
        raise TokenNoValido("TOKEN_MALFORMADO", "Token mal formado.") from None
    if not isinstance(cab, dict) or not isinstance(cl, dict) or cab.get("alg") != "EdDSA":
        raise TokenNoValido("TOKEN_MALFORMADO", "Algoritmo no admitido.")
    claves = claves_publicas() if claves is None else claves
    clave = claves.get(str(cab.get("kid", "")))
    if not clave:
        raise TokenNoValido("CLAVE_DESCONOCIDA", "Clave de firma desconocida.")
    try:
        clave.verify(firma, f"{partes[0]}.{partes[1]}".encode())
    except InvalidSignature:
        raise TokenNoValido("FIRMA_NO_VALIDA", "Firma no válida.") from None
    t = time.time() if ahora is None else ahora
    try:
        iat, exp = int(cl["iat"]), int(cl["exp"])
    except (KeyError, TypeError, ValueError):
        raise TokenNoValido("TOKEN_MALFORMADO", "Faltan fechas.") from None
    # sin margen en la caducidad (auditoría 10-oct-2026: con él un token de 60 s valía 90 s); el margen de reloj
    # solo para la emisión
    if exp <= iat or exp - iat > MAX_VIDA or exp < t or iat > t + MARGEN:
        raise TokenNoValido("TOKEN_CADUCADO", "Token caducado.")
    if cl.get("iss") != ISS or cl.get("aud") != aud:
        raise TokenNoValido("AUDIENCIA", "El token no es para este servicio.")
    if cl.get("htm") != metodo.upper() or cl.get("htu") != htu:
        raise TokenNoValido("PETICION", "El token es de otra petición.")
    if cl.get("bh") != hashlib.sha256(cuerpo or b"").hexdigest():
        raise TokenNoValido("PETICION", "El cuerpo no coincide con el token.")
    if not isinstance(cl.get("scope"), list) or not cl.get("sub") or not isinstance(cl.get("jti"), str):
        raise TokenNoValido("TOKEN_MALFORMADO", "Faltan datos.")
    jti = cl["jti"]
    if not (16 <= len(jti) <= 64 and jti.replace("-", "").replace("_", "").isalnum()):
        raise TokenNoValido("TOKEN_MALFORMADO", "jti no válido.")
    if not _jti_nuevo(aud, jti):
        raise TokenNoValido("REPETIDO", "Token ya usado.")
    return cl


def exige(claims, permiso):
    if permiso not in claims.get("scope", []):
        raise TokenNoValido("SCOPE", f"Falta el permiso {permiso}.")
