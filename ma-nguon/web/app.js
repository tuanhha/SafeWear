/* SafeWear data adapter and operator UI. No telemetry is fabricated in this client. */
'use strict';
const byId = id => document.getElementById(id);
const cards = new Map();
const history = new Map();
let lastMessage = 0, lastState = null, socket = null;
const states = {ALARM:'Chờ tiếp nhận',ACKNOWLEDGED:'Đã tiếp nhận',RESOLVED:'Đã xử lý'};
const kinds = {MANUAL_SOS:'Gọi hỗ trợ chủ động',FALL_SUSPECT:'Nghi té ngã'};
const labels = {idle:'Ít vận động',rest:'Ít vận động',walking:'Đi bộ',running:'Chạy',fall:'Nghi té ngã',unknown:'Chưa đủ dữ liệu'};
function text(id,value){byId(id).textContent=value;}
function setText(root,selector,value){root.querySelector(selector).textContent=value;}
function age(ms){return ms==null?'—':ms<1000?'Vừa nhận':`${Math.floor(ms/1000)} giây trước`;}
function chart(svg,points){
  svg.replaceChildren();
  const valid = points.filter(x=>Number.isFinite(x));
  if(valid.length<2)return;
  const low=Math.min(...valid)-5, high=Math.max(...valid)+5;
  let d='',pen=false;
  points.forEach((value,i)=>{
    if(!Number.isFinite(value)){pen=false;return;}
    const x=(i/Math.max(1,points.length-1))*296+2,y=44-(value-low)/(high-low)*37;
    d+=`${pen?'L':'M'}${x.toFixed(1)},${y.toFixed(1)} `;pen=true;
  });
  const path=document.createElementNS('http://www.w3.org/2000/svg','path');
  path.setAttribute('d',d);path.setAttribute('fill','none');path.setAttribute('stroke','#8bbda0');path.setAttribute('stroke-width','1.8');
  svg.append(path);
}
function renderNode(node){
  let card=cards.get(node.id);
  if(!card){card=byId('worker-template').content.firstElementChild.cloneNode(true);cards.set(node.id,card);byId('worker-list').append(card);}
  setText(card,'.worker-name',node.name);setText(card,'.worker-avatar',String(node.id).padStart(2,'0'));
  const online=node.connection==='online', badge=card.querySelector('.node-status');
  badge.className=`node-status status-badge ${online?'online':'offline'}`;badge.textContent=online?'Đang kết nối':'Mất kết nối';
  setText(card,'.hr',node.hr==null?'—':Math.round(node.hr));setText(card,'.spo2',node.spo2==null?'—':node.spo2.toFixed(1));
  const bp=node.blood_pressure||{},bpOk=Number.isFinite(bp.systolic_mmhg)&&Number.isFinite(bp.diastolic_mmhg);
  setText(card,'.bp',bpOk?`${Math.round(bp.systolic_mmhg)}/${Math.round(bp.diastolic_mmhg)}`:'—');
  const bpStates={model_missing:'Chưa có mô hình',model_invalid:'Mô hình không hợp lệ',uncalibrated:'Chưa hiệu chuẩn cho người đeo',signal_invalid:'Chưa có tín hiệu hợp lệ',prediction_invalid:'Kết quả không hợp lệ',no_signal:'Chưa nhận đặc trưng PPG',stale:'Số đo đã hết thời hạn',simulated:'Dữ liệu mô phỏng',out_of_domain:'Ngoài miền dữ liệu đã thử nghiệm'};
  setText(card,'.bp-quality',bpOk?`Thử nghiệm · ${age(bp.age_ms)}`:(bpStates[bp.status]||'Chưa đủ điều kiện ước lượng'));
  setText(card,'.bp-time',bp.received_at?`Nhận lúc ${new Date(bp.received_at).toLocaleString('vi-VN')}`:'Chưa có phiên đo');
  for(const signal of ['hr','spo2']){
    const element=card.querySelector(`.${signal}-quality`),valid=node[signal]!=null;
    element.className=`quality ${signal}-quality ${valid?'valid':''}`;
    element.textContent=valid?`Tín hiệu hợp lệ · ${age(node[`${signal}_age_ms`])}`:online?'Chưa có tín hiệu hợp lệ':'Số đo đã hết thời hạn';
  }
  setText(card,'.activity',node.activity_source==='untrained'?'Chưa có mô hình':labels[node.activity]||node.activity);
  setText(card,'.battery',node.battery_mv==null?'Chưa đo':`${(node.battery_mv/1000).toFixed(2)} V`);
  setText(card,'.rssi',node.rssi==null?'Chưa có':`${Math.round(node.rssi)} dBm`);
  setText(card,'.last-seen',age(node.last_seen_age_ms));
  let h=history.get(node.id);
  if(!h){h={sequence:null,points:[]};history.set(node.id,h);}
  if(h.sequence!==node.sample_sequence){h.points.push(node.hr);if(h.points.length>120)h.points.shift();h.sequence=node.sample_sequence;}
  if(node.hr==null&&h.points.length&&h.points.at(-1)!==null)h.points.push(null);
  chart(card.querySelector('.sparkline'),h.points);
}
async function post(url,body){
  const response=await fetch(url,{method:'POST',headers:body?{'Content-Type':'application/json'}:{},body:body?JSON.stringify(body):undefined});
  if(!response.ok){const err=await response.json().catch(()=>({}));throw Error(err.detail||'Không thực hiện được thao tác.');}
  return response.json();
}
function renderEvents(events){
  const target=byId('event-list');target.replaceChildren();
  if(!events.length){const tr=document.createElement('tr'),td=document.createElement('td');td.colSpan=5;td.className='table-empty';td.textContent='Chưa có cảnh báo.';tr.append(td);target.append(tr);return;}
  events.forEach(event=>{
    const row=document.createElement('tr');
    const kind=document.createElement('td'),title=document.createElement('span'),code=document.createElement('span');
    title.className='event-kind';title.textContent=kinds[event.kind]||event.kind;code.className='event-code';code.textContent=event.event_id+(event.simulated?' · MÔ PHỎNG':'');kind.append(title,code);
    const wearer=document.createElement('td');wearer.textContent=`Người đeo ${String(event.node_id).padStart(2,'0')}`;
    const date=document.createElement('td');date.textContent=new Date(event.received_at).toLocaleString('vi-VN');
    const state=document.createElement('td'),pill=document.createElement('span');pill.className=`event-state ${event.state}`;pill.textContent=states[event.state]||event.state;state.append(pill);
    const actions=document.createElement('td');
    for(const action of event.state==='ALARM'?['ack','resolve']:event.state==='ACKNOWLEDGED'?['resolve']:[]){
      const button=document.createElement('button');button.className=`button ${action==='resolve'?'resolve':''}`;button.textContent=action==='ack'?'Tiếp nhận':'Đã xử lý';
      button.addEventListener('click',async()=>{button.disabled=true;try{await post(`/api/events/${encodeURIComponent(event.event_id)}/${action}`);text('action-message','Đã lưu thao tác.');await refresh();}catch(err){text('action-message',err.message);}finally{button.disabled=false;}});actions.append(button);
    }
    row.append(kind,wearer,date,state,actions);target.append(row);
  });
}
function render(data){
  lastState=data;
  const demo=data.mode==='demo',source=data.source_status;
  byId('mode-banner').className=`mode-banner ${demo?'demo':'hardware'}`;
  text('mode-banner',demo?'CHẾ ĐỘ MÔ PHỎNG — Dữ liệu thử, không phải số đo trên người.':source.transport==='wired'?'ĐO QUA DÂY — Kiểm tra bàn; chưa phải phép thử đường truyền LoRa.':'CHẾ ĐỘ PHẦN CỨNG — Chỉ hiển thị dữ liệu nhận được từ thiết bị.');
  byId('demo-panel').hidden=!demo;
  byId('storage-banner').hidden=!(data.diagnostics.volatile_alarms>0);
  text('online-count',data.nodes.filter(n=>n.connection==='online').length);text('total-count',`/ ${data.nodes.length} thiết bị`);
  const open=data.events.filter(e=>e.state!=='RESOLVED');text('open-count',open.length);
  const nodeLink=source.transport==='wired'?'Dây UART':'LoRa';
  const uplink=source.uplink==='http'?'Wi-Fi / HTTP':source.uplink==='uart'?'UART GPIO':'USB';
  text('transport',source.gateway==='offline'?'Chưa kết nối':demo?'LoRa (mô phỏng)':`${nodeLink} → ${uplink}`);
  text('gateway-status',source.gateway==='online'?(source.transport==='wired'?'Gateway nối dây đang hoạt động':source.radio_ok?'Gateway đang hoạt động':'Gateway báo lỗi radio'):'Chưa có heartbeat mới từ Gateway');
  const model=data.model||{};text('model-status',model.state==='trained_experimental'?'RF thử nghiệm':model.state==='synthetic'?'RF từ dữ liệu giả lập':model.state==='invalid_model'?'Mô hình không hợp lệ':'Chưa huấn luyện');
  byId('empty-state').hidden=!!data.nodes.length;
  data.nodes.forEach(renderNode);
  for(const [id,card]of cards){if(!data.nodes.some(n=>n.id===id)){card.remove();cards.delete(id);history.delete(id);}}
  byId('alarm-banner').hidden=!open.length;text('alarm-text',`${open.length} yêu cầu chưa kết thúc. Kiểm tra người đeo và xác nhận xử lý.`);
  text('event-total',`${data.events.length} sự kiện`);renderEvents(data.events);
  const diagnostic=byId('diagnostic-content');diagnostic.replaceChildren();
  const lines=[`Khung đã nhận: ${data.diagnostics.received_frames||0} · Khung bị loại: ${data.diagnostics.rejected_frames||0} · Sự kiện trùng: ${data.diagnostics.duplicate_events||0}`,`Node → Gateway: ${nodeLink} · Gateway → Pi: ${uplink} · Gateway: ${source.gateway} · Mô hình: ${model.state||'untrained'}`,`Gói HTTP đã lưu: ${data.diagnostics.http_accepted||0} · Gửi lại đã nhận trước: ${data.diagnostics.http_duplicates||0}`,`Lỗi gần nhất: ${data.diagnostics.last_error||'Không ghi nhận'}`];
  lines.forEach(line=>{const p=document.createElement('div');p.textContent=line;diagnostic.append(p);});
}
async function refresh(){try{const response=await fetch('/api/latest');if(response.ok)render(await response.json());}catch(_){}}
function connect(){
  socket=new WebSocket(`${location.protocol==='https:'?'wss':'ws'}://${location.host}/ws`);
  socket.onmessage=event=>{try{const data=JSON.parse(event.data);lastMessage=Date.now();byId('web-status').className='connection-pill connected';text('web-status','● Dịch vụ đang hoạt động');render(data);}catch(_){text('action-message','Thông báo từ dịch vụ không hợp lệ.');}};
  socket.onclose=()=>{byId('web-status').className='connection-pill disconnected';text('web-status','Mất kết nối dịch vụ');setTimeout(connect,1500);};
  socket.onerror=()=>socket.close();
}
document.querySelectorAll('[data-demo]').forEach(button=>button.addEventListener('click',async()=>{button.disabled=true;try{render(await post('/api/demo/action',{action:button.dataset.demo}));text('action-message','Đã thực hiện tình huống mô phỏng.');}catch(err){text('action-message',err.message);}finally{button.disabled=false;}}));
setInterval(()=>{
  text('clock',new Date().toLocaleString('vi-VN',{hour:'2-digit',minute:'2-digit',day:'2-digit',month:'2-digit',year:'numeric'}));
  if(lastMessage&&Date.now()-lastMessage>3500&&lastState){
    const stale=structuredClone(lastState);stale.source_status.gateway='offline';stale.nodes.forEach(n=>{n.hr=null;n.spo2=null;n.connection='offline';n.blood_pressure={...n.blood_pressure,status:'stale',systolic_mmhg:null,diastolic_mmhg:null};});render(stale);
  }
},1000);
refresh();connect();
