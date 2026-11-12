package com.yieyu.ultimate.service

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.graphics.drawable.Icon
import android.media.session.MediaSession
import android.media.session.PlaybackState
import android.os.Build
import android.os.IBinder

/**
 * 【模块 22】前台服务保活 —— 全场唯一 Kotlin。
 *
 * 只干一件事：让系统知道"我在放音乐"，从而不被杀。
 * 业务判定（能不能播、播什么、卡密是否有效）一律不在这里。
 * 与 C++ 的接口只有 2 个符号（core/src/jsb/jni_service.cpp，唯一允许的 JNI 面）。
 *
 * 注意两个"码"不要混（踩过）：
 *   · ACTION_*  = Intent action 字符串（系统用）
 *   · CODE_*    = 传给 native 的整数编号（我们自己用）
 */
class PlaybackService : Service() {

    private var session: MediaSession? = null

    override fun onCreate() {
        super.onCreate()
        createChannel()
        session = MediaSession(this, "YieyuUltimate").apply {
            setCallback(object : MediaSession.Callback() {
                override fun onPlay() = dispatch(CODE_PLAY)
                override fun onPause() = dispatch(CODE_PAUSE)
                override fun onSkipToNext() = dispatch(CODE_NEXT)
                override fun onSkipToPrevious() = dispatch(CODE_PREV)
            })
            isActive = true
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_PLAY -> dispatch(CODE_PLAY)
            ACTION_PAUSE -> dispatch(CODE_PAUSE)
            ACTION_NEXT -> dispatch(CODE_NEXT)
            ACTION_PREV -> dispatch(CODE_PREV)
            else -> Unit
        }
        startForeground(NOTIF_ID, buildNotification(isPlaying()))
        return START_STICKY
    }

    override fun onBind(intent: Intent?): IBinder? = null

    /** 只转发，不判定。真实状态由 core 通过 JSB 广播回 Flutter。 */
    private fun dispatch(code: Int) {
        nativeAction(code)
        val playing = code == CODE_PLAY
        (getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager)
            .notify(NOTIF_ID, buildNotification(playing))
        session?.setPlaybackState(
            PlaybackState.Builder()
                .setActions(PlaybackState.ACTION_PLAY or PlaybackState.ACTION_PAUSE)
                .setState(
                    if (playing) PlaybackState.STATE_PLAYING else PlaybackState.STATE_PAUSED,
                    PlaybackState.PLAYBACK_POSITION_UNKNOWN,
                    1.0f,
                )
                .build(),
        )
    }

    private fun isPlaying(): Boolean = nativePing() == 1

    private fun createChannel() {
        val ch = NotificationChannel(CHANNEL_ID, "播放", NotificationManager.IMPORTANCE_LOW).apply {
            setShowBadge(false)
            description = "播放控制与进度"
        }
        (getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager).createNotificationChannel(ch)
    }

    private fun buildNotification(playing: Boolean): Notification {
        val open = packageManager.getLaunchIntentForPackage(packageName)
            ?: Intent(this, PlaybackService::class.java)   // 兜底：launch intent 可能为 null（踩过）

        val contentPi = PendingIntent.getActivity(
            this, 0, open,
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )

        val b = Notification.Builder(this, CHANNEL_ID)
            .setContentTitle("Yieyu Ultimate Player")
            .setContentText(if (playing) "正在播放" else "已暂停")
            .setSmallIcon(android.R.drawable.ic_media_play)
            .setContentIntent(contentPi)
            .setOngoing(playing)
            .setVisibility(Notification.VISIBILITY_PUBLIC)

        b.addAction(action(android.R.drawable.ic_media_previous, "上一首", ACTION_PREV, 1))
        if (playing) {
            b.addAction(action(android.R.drawable.ic_media_pause, "暂停", ACTION_PAUSE, 2))
        } else {
            b.addAction(action(android.R.drawable.ic_media_play, "播放", ACTION_PLAY, 2))
        }
        b.addAction(action(android.R.drawable.ic_media_next, "下一首", ACTION_NEXT, 3))

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            b.setForegroundServiceBehavior(Notification.FOREGROUND_SERVICE_IMMEDIATE)
        }
        return b.build()
    }

    private fun action(icon: Int, title: String, actionString: String, reqCode: Int): Notification.Action {
        val pi = PendingIntent.getService(
            this, reqCode,
            Intent(this, PlaybackService::class.java).setAction(actionString),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        return Notification.Action.Builder(Icon.createWithResource(this, icon), title, pi).build()
    }

    // ── 唯一允许的 JNI 面（2 个符号）───────────────────────────────
    private external fun nativePing(): Int
    private external fun nativeAction(code: Int)

    companion object {
        private const val CHANNEL_ID = "yieyu.playback"
        private const val NOTIF_ID = 0x5945

        // Intent action（字符串）
        const val ACTION_PLAY = "com.yieyu.ultimate.action.PLAY"
        const val ACTION_PAUSE = "com.yieyu.ultimate.action.PAUSE"
        const val ACTION_NEXT = "com.yieyu.ultimate.action.NEXT"
        const val ACTION_PREV = "com.yieyu.ultimate.action.PREV"

        // native 码（与 core 的 Yieyu_Service_OnNotificationAction 约定）
        const val CODE_PLAY = 0
        const val CODE_PAUSE = 1
        const val CODE_NEXT = 2
        const val CODE_PREV = 3

        init {
            System.loadLibrary("yieyu_core")
        }

        fun start(ctx: Context) {
            val i = Intent(ctx, PlaybackService::class.java)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) ctx.startForegroundService(i) else ctx.startService(i)
        }
    }
}
