import server from './src/server.js';

server.listen(Number(process.env.PORT || 8080), '127.0.0.1', () => {
  console.log('Local backend: http://127.0.0.1:' + server.address().port);
});
