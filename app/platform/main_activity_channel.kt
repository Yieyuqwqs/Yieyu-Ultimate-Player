
    // ── 平台通道（取代 file_picker / path_provider：这两个第三方插件会拖 SDK 版本依赖）──
    private var pendingPick: MethodChannel.Result? = null

    private fun setupYieyuChannel() {
        val engine = flutterEngine ?: return
        MethodChannel(engine.dartExecutor.binaryMessenger, "yieyu/native")
            .setMethodCallHandler { call, result ->
                when (call.method) {
                    "appDir" -> result.success(filesDir.absolutePath)
                    "pickFile" -> {
                        pendingPick = result
                        val i = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
                            addCategory(Intent.CATEGORY_OPENABLE)
                            type = "*/*"
                        }
                        try {
                            startActivityForResult(i, REQ_PICK)
                        } catch (t: Throwable) {
                            pendingPick = null
                            result.success(null)
                        }
                    }
                    else -> result.notImplemented()
                }
            }
    }

    private fun pickFileName(uri: Uri): String? {
        return try {
            contentResolver.query(uri, null, null, null, null)?.use { c ->
                if (!c.moveToFirst()) return@use null
                val idx = c.getColumnIndex(android.provider.OpenableColumns.DISPLAY_NAME)
                if (idx >= 0) c.getString(idx) else null
            }
        } catch (_: Throwable) {
            null
        }
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != REQ_PICK) return
        val r = pendingPick ?: return
        pendingPick = null
        val uri = data?.data
        if (resultCode != Activity.RESULT_OK || uri == null) {
            r.success(null)
            return
        }
        try {
            val name = pickFileName(uri) ?: "picked.bin"
            val bytes = contentResolver.openInputStream(uri)?.use { it.readBytes() } ?: ByteArray(0)
            r.success(mapOf("name" to name, "bytes" to bytes))
        } catch (t: Throwable) {
            r.success(null)
        }
    }

    companion object {
        private const val REQ_PICK = 0x5945
    }
