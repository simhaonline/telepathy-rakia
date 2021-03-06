import dbus

from sofiatest import exec_test

from servicetest import (
    make_channel_proxy, wrap_channel, sync_dbus,
    EventPattern, call_async, ProxyWrapper,
    assertEquals, assertNotEquals, assertContains, assertLength,
    )
import constants as cs
from voip_test import VoipTestContext

class CallTest:
    def __init__(self, q, bus, conn, sip_proxy, incoming, audio=True,
                 video=False, peer='foo@bar.com'):
        self.q = q
        self.bus = bus
        self.conn = conn
        self.sip_proxy = sip_proxy
        self.incoming = incoming
        self.peer = peer
        self.content_count = 0;
        if audio:
            self.content_count += 1
            if incoming:
                self.initial_audio_content_name = 'initial_audio_' + str(self.content_count)
            else:
                self.initial_audio_content_name = 'initialaudio'
        else:
            self.initial_audio_content_name = None
        if video:
            self.content_count += 1
            if incoming:
                self.initial_video_content_name = 'initial_video_' + str(self.content_count)
            else:
                self.initial_video_content_name = 'initialvideo'
        else:
            self.initial_video_content_name = None
        self.contents = []

        self.medias = []
        if self.initial_audio_content_name:
            self.add_to_medias('audio')
        if self.initial_video_content_name:
            self.add_to_medias('video')


    def add_to_medias(self, mediatype, direction=None):
        for i in range(0, len(self.medias)):
            if self.medias[i][1] == 0:
                self.medias[i] = (mediatype, direction)
                return
        self.medias += [(mediatype, direction)]

    def connect(self):
        self.conn.Connect()
        self.q.expect('dbus-signal', signal='StatusChanged', args=[0, 1])

        self.context = VoipTestContext(self.q, self.conn, self.bus,
                                       self.sip_proxy,
                                       'sip:testacc@127.0.0.1', self.peer)
        self.self_handle = self.conn.Get(cs.CONN, 'SelfHandle', dbus_interface=cs.PROPERTIES_IFACE)
        self.remote_handle = self.conn.get_contact_handle_sync(self.context.peer)

    def check_channel_props(self, props, initial):
        assertEquals(cs.CHANNEL_TYPE_CALL, props[cs.CHANNEL_TYPE])
        assertEquals(cs.HT_CONTACT, props[cs.CHANNEL + '.TargetHandleType'])
        assertEquals(self.remote_handle, props[cs.CHANNEL + '.TargetHandle'])
        if self.incoming:
            assertEquals(self.remote_handle,
                         props[cs.CHANNEL + '.InitiatorHandle'])
        else:
            assertEquals(self.self_handle,
                         props[cs.CHANNEL + '.InitiatorHandle'])
        if initial and self.initial_audio_content_name is not None:
            assertEquals(True, props[cs.CHANNEL_TYPE_CALL + '.InitialAudio'])
            assertEquals(self.initial_audio_content_name,
                         props[cs.CHANNEL_TYPE_CALL + '.InitialAudioName'])
        else:
            assertEquals(False, props[cs.CHANNEL_TYPE_CALL + '.InitialAudio'])
        if initial and self.initial_video_content_name is not None:
            assertEquals(True, props[cs.CHANNEL_TYPE_CALL + '.InitialVideo'])
            assertEquals(self.initial_video_content_name,
                         props[cs.CHANNEL_TYPE_CALL + '.InitialVideoName'])
        else:
            assertEquals(False, props[cs.CHANNEL_TYPE_CALL + '.InitialVideo'])
        assertEquals(True, props[cs.CHANNEL_TYPE_CALL + '.MutableContents'])
        assertEquals(False, props[cs.CHANNEL_TYPE_CALL + '.HardwareStreaming'])

    def check_endpoint(self, content, endpoint_path):        
        endpoint = self.bus.get_object(self.conn.bus_name, endpoint_path)
        endpoint_props = endpoint.GetAll(cs.CALL_STREAM_ENDPOINT)
        assertEquals(('',''), endpoint_props['RemoteCredentials'])
        assertEquals(self.context.get_remote_candidates_dbus(),
                     endpoint_props['RemoteCandidates'])
        assertLength(0, endpoint_props['EndpointState'])
        assertEquals(cs.CALL_STREAM_TRANSPORT_RAW_UDP,
                     endpoint_props['Transport'])
        assertEquals(False, endpoint_props['IsICELite'])


    def __add_stream (self, content, stream_path, initial, incoming):
        tmpstream = self.bus.get_object (self.conn.bus_name, stream_path)

        content.stream = ProxyWrapper (tmpstream, cs.CALL_STREAM,
                           {'Media': cs.CALL_STREAM_IFACE_MEDIA})

        stream_props = content.stream.Properties.GetAll(cs.CALL_STREAM)
        assertEquals(True, stream_props['CanRequestReceiving'])
        if incoming:
            assertEquals(cs.CALL_SENDING_STATE_PENDING_SEND,
                         stream_props['LocalSendingState'])
        else:
            assertEquals(cs.CALL_SENDING_STATE_SENDING,
                         stream_props['LocalSendingState'])

        if incoming:
            assertEquals(
                {self.remote_handle: cs.CALL_SENDING_STATE_SENDING},
                stream_props['RemoteMembers'])
        else:
            assertEquals(
                {self.remote_handle: cs.CALL_SENDING_STATE_PENDING_SEND},
                stream_props['RemoteMembers'])

        smedia_props = content.stream.Properties.GetAll(
            cs.CALL_STREAM_IFACE_MEDIA)
        assertEquals(cs.CALL_SENDING_STATE_NONE, smedia_props['SendingState'])
        if initial:
            assertEquals(cs.CALL_SENDING_STATE_NONE,
                         smedia_props['ReceivingState'])
        else:
            assertEquals(cs.CALL_SENDING_STATE_PENDING_SEND,
                         smedia_props['ReceivingState'])
        assertEquals(cs.CALL_STREAM_TRANSPORT_RAW_UDP,
                     smedia_props['Transport'])
        assertEquals([], smedia_props['LocalCandidates'])
        assertEquals(("",""), smedia_props['LocalCredentials'])
        assertEquals([], smedia_props['STUNServers'])
        assertEquals([], smedia_props['RelayInfo'])
        assertEquals(True, smedia_props['HasServerInfo'])
        if incoming:
            assertLength(1, smedia_props['Endpoints'])
            self.check_endpoint(content, smedia_props['Endpoints'][0])

        else:
            assertEquals([], smedia_props['Endpoints'])        
        assertEquals(False, smedia_props['ICERestartPending'])

    def get_md(self, content):
        if content.media_type == cs.MEDIA_STREAM_TYPE_AUDIO:
            return context.get_audio_md_dbus(self.remote_handle)
        elif content.media_type == cs.MEDIA_STREAM_TYPE_VIDEO:
            return context.get_video_md_dbus(self.remote_handle)
        else:
            assert False

    def add_content(self, content_path, initial = False, incoming = None):

        if initial:
            incoming = self.incoming
        else:
            assert incoming is not None
        
        content = self.bus.get_object (self.conn.bus_name, content_path)
        
        content_props = content.GetAll(cs.CALL_CONTENT)
        if initial:
            assertEquals(cs.CALL_DISPOSITION_INITIAL,
            content_props['Disposition'])
            if content_props['Type'] == cs.MEDIA_STREAM_TYPE_AUDIO:
                assertEquals(self.initial_audio_content_name,
                             content_props['Name'])
            elif content_props['Type'] == cs.MEDIA_STREAM_TYPE_VIDEO:
                assertEquals(self.initial_video_content_name,
                             content_props['Name'])
            else:
                assert Fale

        else:
            assertEquals(cs.CALL_DISPOSITION_NONE,
            content_props['Disposition'])

        content.media_type = content_props['Type']

        cmedia_props = content.GetAll(cs.CALL_CONTENT_IFACE_MEDIA)
        assertLength(0, cmedia_props['RemoteMediaDescriptions'])
        assertLength(0, cmedia_props['LocalMediaDescriptions'])
        if incoming:
            assertNotEquals('/', cmedia_props['MediaDescriptionOffer'][0])
        else:
            assertNotEquals('/', cmedia_props['MediaDescriptionOffer'][0])
        assertEquals(cs.CALL_CONTENT_PACKETIZATION_RTP,
        cmedia_props['Packetization'])
        assertEquals(cs.CALL_SENDING_STATE_NONE, cmedia_props['CurrentDTMFState'])
        
        self.contents.append(content)

        self.__add_stream(content, content_props['Streams'][0], initial,
                          incoming)

        if incoming:
            md = self.bus.get_object (self.conn.bus_name,
                                 cmedia_props['MediaDescriptionOffer'][0])
            md.Accept(self.context.get_audio_md_dbus(self.remote_handle))
            o = self.q.expect_many(
                EventPattern('dbus-signal', signal='MediaDescriptionOfferDone'),
                EventPattern('dbus-signal', signal='LocalMediaDescriptionChanged'),
                EventPattern('dbus-signal', signal='RemoteMediaDescriptionsChanged'))

        return content                   

    def check_call_properties(self, call_props):
        if self.incoming:
            assertEquals(cs.CALL_STATE_INITIALISED, call_props['CallState'])
        else:
            assertEquals(cs.CALL_STATE_PENDING_INITIATOR,
                         call_props['CallState'])
        assertEquals(0, call_props['CallFlags'])
        assertEquals(False, call_props['HardwareStreaming'])
        assertEquals(True, call_props['MutableContents'])
        assertEquals(self.initial_audio_content_name is not None,
                     call_props['InitialAudio'])
        assertEquals(self.initial_audio_content_name or "",
                     call_props['InitialAudioName'])
        assertEquals(self.initial_video_content_name is not None,
                     call_props['InitialVideo'])
        assertEquals(self.initial_video_content_name or "",
                     call_props['InitialVideoName'])
        assertEquals(cs.CALL_STREAM_TRANSPORT_RAW_UDP,
                     call_props['InitialTransport'])
        assertEquals({self.remote_handle: 0}, call_props['CallMembers'])


        assertLength(self.content_count, call_props['Contents'])


    def initiate(self):
        if self.incoming:
            self.context.incoming_call(self.medias)
        else:
            self.chan_path = self.conn.Requests.CreateChannel({
                    cs.CHANNEL_TYPE: cs.CHANNEL_TYPE_CALL,
                    cs.TARGET_HANDLE_TYPE: cs.HT_CONTACT,
                    cs.TARGET_HANDLE: self.remote_handle,
                    cs.CALL_INITIAL_AUDIO: self.initial_audio_content_name is not None,
                    cs.CALL_INITIAL_AUDIO_NAME: self.initial_audio_content_name or "",
                    cs.CALL_INITIAL_VIDEO: self.initial_video_content_name is not None,
                    cs.CALL_INITIAL_VIDEO_NAME: self.initial_video_content_name or "",
                    })[0]

        nc = self.q.expect('dbus-signal', signal='NewChannels')

        assertLength(1, nc.args)
        assertLength(1, nc.args[0])       # one channel
        assertLength(2, nc.args[0][0])    # two struct members
        self.chan_path, props = nc.args[0][0]
        self.check_channel_props(props, True)
                
        self.chan = wrap_channel(
            self.bus.get_object(self.conn.bus_name, self.chan_path), 'Call1',
            ['Hold'])
            
        call_props = self.chan.Properties.GetAll(cs.CHANNEL_TYPE_CALL)
        self.check_call_properties(call_props)
        for c in call_props['Contents']:
            self.add_content(c, True)

        if not self.incoming:
            self.chan.Call1.Accept()

            self.q.expect_many(
                *self.stream_dbus_signal_event('ReceivingStateChanged',
                                                   args=[cs.CALL_STREAM_FLOW_STATE_PENDING_START]))

            for c in self.contents:
                c.stream.Media.CompleteReceivingStateChange(
                    cs.CALL_STREAM_FLOW_STATE_STARTED)

                mdo = c.Get(cs.CALL_CONTENT_IFACE_MEDIA,
                            'MediaDescriptionOffer')
                md = self.bus.get_object (self.conn.bus_name, mdo[0])
                md.Accept(self.context.get_audio_md_dbus(
                        self.remote_handle))

                self.q.expect_many(
                    EventPattern('dbus-signal', signal='MediaDescriptionOfferDone',
                                 path=c.__dbus_object_path__),
                    EventPattern('dbus-signal', signal='LocalMediaDescriptionChanged',
                                 path=c.__dbus_object_path__),
                    EventPattern('dbus-signal', signal='RemoteMediaDescriptionsChanged',
                                 path=c.__dbus_object_path__))

                mdo = c.Get(cs.CALL_CONTENT_IFACE_MEDIA,
                            'MediaDescriptionOffer')
                assertEquals(('/', {}), mdo)

                self.add_candidates(c.stream)
                
            self.invite_event = self.q.expect('sip-invite')

    def content_dbus_signal_event(self, s, **kwparams):
        return map(
            lambda c:
                EventPattern('dbus-signal', signal=s,
                             path=c.__dbus_object_path__,
                             **kwparams),
            self.contents)

    def stream_dbus_signal_event(self, s, **kwparams):
        return map(
            lambda c:
                EventPattern('dbus-signal', signal=s,
                             path=c.stream.__dbus_object_path__,
                             **kwparams),
            self.contents)
                
    def add_candidates(self, stream):
        stream.Media.AddCandidates(self.context.get_remote_candidates_dbus())
        stream.Media.FinishInitialCandidates()
        
        self.q.expect('dbus-signal', signal='LocalCandidatesAdded',
                 path=stream.__dbus_object_path__)

    def accept_incoming(self):
        if not self.incoming:
            return

        self.chan.Call1.Accept()

        events = self.stream_dbus_signal_event(
            'ReceivingStateChanged',
            args=[cs.CALL_STREAM_FLOW_STATE_PENDING_START]) + \
            self.stream_dbus_signal_event(
                'SendingStateChanged',
                args=[cs.CALL_STREAM_FLOW_STATE_PENDING_START])
        o = self.q.expect_many(
            EventPattern('dbus-signal', signal='CallStateChanged'),
            EventPattern('dbus-signal', signal='CallStateChanged'),
            *events)
        assertEquals(cs.CALL_STATE_ACCEPTED, o[0].args[0])
        assertEquals(cs.CALL_STATE_ACTIVE, o[1].args[0])


        for c in self.contents:
            c.stream.Media.CompleteReceivingStateChange(
                cs.CALL_STREAM_FLOW_STATE_STARTED)

            #self.q.expect('dbus-signal', signal='ReceivingStateChanged',
            #         args=[cs.CALL_STREAM_FLOW_STATE_STARTED],
            #         path=c.stream.__dbus_object_path__)

        self.q.expect_many(
            *self.stream_dbus_signal_event(
                'LocalSendingStateChanged',
                predicate=lambda e: cs.CALL_SENDING_STATE_SENDING == e.args[0]))

        for c in self.contents:
            c.stream.Media.CompleteSendingStateChange(
            cs.CALL_STREAM_FLOW_STATE_STARTED)

            self.q.expect('dbus-signal', signal='SendingStateChanged',
                          args=[cs.CALL_STREAM_FLOW_STATE_STARTED],
                          path=c.stream.__dbus_object_path__)

            self.add_candidates(c.stream)


        acc = self.q.expect('sip-response', call_id=self.context.call_id,
                            code=200)

        self.context.check_call_sdp(acc.sip_message.body, self.medias)
        self.context.ack(acc.sip_message)

    def accept_outgoing(self):
        if self.incoming:
            return

        self.context.check_call_sdp(self.invite_event.sip_message.body,
                                    self.medias)
        self.context.accept(self.invite_event.sip_message)

        ack_cseq = "%s ACK" % self.invite_event.cseq.split()[0]
        del self.invite_event

        events = self.content_dbus_signal_event('NewMediaDescriptionOffer') + \
            self.stream_dbus_signal_event('EndpointsChanged')
        o = self.q.expect_many(
            EventPattern('sip-ack', cseq=ack_cseq),
            # Call accepted
            *events)

        for i in o:
            if i.type != 'dbus-signal' or \
                    i.signal != 'NewMediaDescriptionOffer':
                continue
            md = self.bus.get_object (self.conn.bus_name, i.args[0])
            md.Accept(self.context.get_audio_md_dbus(self.remote_handle))

        o = self.q.expect_many(
            # Call accepted
            EventPattern('dbus-signal', signal='CallStateChanged'))

        assertEquals(cs.CALL_STATE_ACCEPTED, o[0].args[0])

        for c in self.contents:
            mdo = c.Get(cs.CALL_CONTENT_IFACE_MEDIA, 'MediaDescriptionOffer')
            assertEquals(('/', {}), mdo)


        for c in self.contents:
            c.stream.Media.CompleteSendingStateChange(
                cs.CALL_STREAM_FLOW_STATE_STARTED)
            self.q.expect('dbus-signal', signal='SendingStateChanged',
                          args=[cs.CALL_STREAM_FLOW_STATE_STARTED],
                          path=c.stream.__dbus_object_path__)
        for i in o:
            if i.type == 'dbus-signal' and i.signal == 'EndpointsChanged':
                assertLength(1, i.args[0])
                assertLength(0, i.args[1])
                self.check_endpoint(c, i.args[0][0])

    def accept(self):
        if self.incoming:
            self.accept_incoming()
        else:
            self.accept_outgoing()

    def running_check(self):
        self.context.options_ping(self.q)
        sync_dbus(self.bus, self.q, self.conn)

        for c in self.contents:
            props = c.stream.Properties.GetAll(cs.CALL_STREAM)
            assertEquals(cs.CALL_SENDING_STATE_SENDING,
                         props['LocalSendingState'])
            assertEquals({self.remote_handle: cs.CALL_SENDING_STATE_SENDING},
                         props['RemoteMembers'])

            props = c.stream.Properties.GetAll(cs.CALL_STREAM_IFACE_MEDIA)
            assertEquals(cs.CALL_STREAM_FLOW_STATE_STARTED,
                         props['SendingState'])
            assertEquals(cs.CALL_STREAM_FLOW_STATE_STARTED,
                         props['ReceivingState'])


    def hangup(self):
        if self.incoming:
            bye_msg = self.context.terminate()
    
            o = self.q.expect_many(
                EventPattern('dbus-signal', signal='CallStateChanged',
                             path=self.chan_path),
                EventPattern('sip-response', cseq=bye_msg.headers['cseq'][0]))
            assertEquals(cs.CALL_STATE_ENDED, o[0].args[0])
            assertEquals(0, o[0].args[1])
            assertEquals(self.remote_handle, o[0].args[2][0])
        else:
            self.chan.Call1.Hangup(cs.CALL_STATE_CHANGE_REASON_USER_REQUESTED, "",
                                   "User hangs up")
            ended_event, bye_event = self.q.expect_many(
                EventPattern('dbus-signal', signal='CallStateChanged'),
                EventPattern('sip-bye', call_id=self.context.call_id))
            # Check that we're the actor
            assertEquals(cs.CALL_STATE_ENDED, ended_event.args[0])
            assertEquals(0, ended_event.args[1])
            assertEquals((self.self_handle, cs.CALL_STATE_CHANGE_REASON_USER_REQUESTED, "",
                          "User hangs up"), ended_event.args[2])
    
            # For completeness, reply to the BYE.
            bye_response = self.sip_proxy.responseFromRequest(
                200, bye_event.sip_message)
            self.sip_proxy.deliverResponse(bye_response)

        
    def during_call(self):
        pass

    def run(self):
        self.connect()
        self.initiate()
        self.accept()
        self.running_check()
        self.during_call()
        self.hangup()
        self.chan.Close()

    


def run_call_test(q, bus, conn, sip_proxy, incoming=False, klass=CallTest,
        **params):
    test = klass(q, bus, conn, sip_proxy, incoming, **params)
    test.run()

def run(**params):
    exec_test(lambda q, b, c, s:
                  run_call_test(q, b, c, s, incoming=True, **params))
    exec_test(lambda q, b, c, s:
                  run_call_test(q, b, c, s, incoming=False, **params))

if __name__ == '__main__':
    run()
    run(peer='foo@sip.bar.com')
    run(video=True)
    run(video=True,audio=False)
