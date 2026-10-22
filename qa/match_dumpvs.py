import sys
d=open(sys.argv[1],'rb').read()
print('stage',d[1],'stocks',d[7],'pl/cp',d[4],d[5],'rules',d[3],'time',d[6])
for i in range(4):
    b=0x20+i*0x74; print(i,'pkind',d[b+2],'fkind',hex(d[b+3]),'costume',d[b+6],'stock_count',d[b+0xB],'tag',d[b+0xA])
